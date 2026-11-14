#include "Container/utility/GuiManager.h"

#include "Container/renderer/bim/BimManager.h"

#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

// Execute production widgets with real ImGui input events, without a windowing
// or GPU backend. The table state exposes scroll positions and hit rectangles.
class BimElementBrowserWidgetTests : public ::testing::Test {
protected:
  void SetUp() override {
    previousContext_ = ImGui::GetCurrentContext();
    context_ = ImGui::CreateContext();
    auto &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(1100.0f, 700.0f);
    io.DeltaTime = 1.0f / 60.0f;
    unsigned char *pixels{};
    int width{}, height{};
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    elements_.resize(10'001u);
    for (size_t index = 0; index < elements_.size(); ++index) {
      auto &element = elements_[index];
      element.objectIndex = static_cast<uint32_t>(index * 3u + 7u);
      element.guid = "BIM element " + std::to_string(index);
      element.sourceId = "Source " + std::to_string(index);
      element.type = "IfcWall";
      element.displayName = "Building element " + std::to_string(index);
      element.storeyName = "Upper floor";
      element.materialName = "Concrete";
    }
    elements_.back().displayName = "Unique FinalOnly element";
    inspection_.hasScene = true;
    inspection_.metadataSource = &firstModel_;
    inspection_.metadataRevision = 1u;
    inspection_.elements = elements_;
    inspection_.selectObject = [&](uint32_t object) {
      selected_ = object;
      inspection_.hasSelection = true;
      inspection_.selectedObjectIndex = object;
    };
    inspection_.focusObject = [&](uint32_t object) { focused_ = object; };
  }

  void TearDown() override {
    ImGui::DestroyContext(context_);
    ImGui::SetCurrentContext(previousContext_);
  }

  void frame() {
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(drawSunHours_ || drawRenderSetups_ ? 500.0f : 1080.0f,
                                  ImGui::GetIO().DisplaySize.y - 40.0f));
    ImGui::Begin("BIM browser widget test", nullptr,
                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoResize);
    const auto origin = ImGui::GetCursorScreenPos();
    searchPosition_ = ImVec2(origin.x + ImGui::GetStyle().IndentSpacing + 40.0f,
                             origin.y + ImGui::GetTextLineHeightWithSpacing() +
                                 ImGui::GetFrameHeight() * 0.5f);
    if (drawRenderSetups_) {
      renderSetupsId_ = ImGui::GetID("Render Setups");
      ImGui::SetNextItemOpen(true, ImGuiCond_Always);
      gui_.drawRenderSetupControls();
    } else if (drawSunHours_) {
      ImGui::SetNextItemOpen(true, ImGuiCond_Always);
      gui_.drawBimSunHoursPanel(inspection_);
      const auto min = ImGui::GetItemRectMin();
      const auto max = ImGui::GetItemRectMax();
      lastItemCenter_ = ImVec2((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f);
    } else {
      if (inspection_.hasScene)
        ImGui::SetNextItemOpen(browserOpen_, ImGuiCond_Always);
      gui_.drawBimElementBrowser(inspection_);
    }
    ImGui::End();
    ImGui::Render();
  }

  void settle() {
    frame();
    frame();
  }

  ImGuiTable *table() {
    for (int index = 0; index < context_->Tables.GetMapSize(); ++index) {
      auto *candidate = context_->Tables.TryGetMapData(index);
      if (candidate != nullptr && candidate->ColumnsCount == 7)
        return candidate;
    }
    return nullptr;
  }

  void click(ImVec2 position) {
    auto &io = ImGui::GetIO();
    io.AddMousePosEvent(position.x, position.y);
    frame();
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    frame();
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    frame();
  }

  ImVec2 renderSetupControl(const char *label, float x = 40.0f) {
    const ImGuiID id = ImHashStr(label, 0, renderSetupsId_);
    for (float y = 30.0f; y < 500.0f; y += 4.0f) {
      ImGui::GetIO().AddMousePosEvent(x, y);
      frame();
      if (context_->HoveredId == id) {
        return ImVec2(x, y);
      }
    }
    ADD_FAILURE() << "Could not find Render Setups widget " << label;
    return ImVec2(-100.0f, -100.0f);
  }

  void typeQuery(std::string_view text) {
    click(searchPosition_);
    auto &io = ImGui::GetIO();
    io.AddKeyEvent(ImGuiMod_Ctrl, true);
    io.AddKeyEvent(ImGuiKey_A, true);
    frame();
    io.AddKeyEvent(ImGuiKey_A, false);
    io.AddKeyEvent(ImGuiMod_Ctrl, false);
    frame();
    const std::string query(text);
    if (text.empty()) {
      io.AddKeyEvent(ImGuiKey_Backspace, true);
      frame();
      io.AddKeyEvent(ImGuiKey_Backspace, false);
    } else {
      io.AddInputCharactersUTF8(query.c_str());
    }
    settle();
    ASSERT_NE(context_->ActiveId, 0u);
    ASSERT_EQ(context_->InputTextState.ID, context_->ActiveId);
    ASSERT_NE(context_->InputTextState.TextA.Data, nullptr);
    EXPECT_STREQ(context_->InputTextState.TextA.Data, query.c_str());
  }

  void selectLastVisibleRow() {
    auto *current = table();
    ASSERT_NE(current, nullptr);
    const float y = (current->RowPosY1 + current->RowPosY2) * 0.5f;
    ASSERT_GT(y, current->InnerWindow->InnerRect.Min.y);
    ASSERT_LT(y, current->InnerWindow->InnerRect.Max.y);
    click(ImVec2(current->Columns[0].WorkMinX + 40.0f, y));
  }

  void focusLastVisibleRow() {
    auto *current = table();
    ASSERT_NE(current, nullptr);
    // Aim at the button's frame, rather than the midpoint of the whole row:
    // a table row may also contain taller text or deferred layout changes.
    const float y = current->RowPosY1 + ImGui::GetStyle().CellPadding.y +
                    ImGui::GetFrameHeight() * 0.5f;
    ASSERT_GT(y, current->InnerWindow->InnerRect.Min.y);
    ASSERT_LT(y, current->InnerWindow->InnerRect.Max.y);
    SCOPED_TRACE(::testing::Message()
                 << "Focus row y=" << y << ", row=" << current->RowPosY1
                 << ".." << current->RowPosY2 << ", x="
                 << current->Columns[1].WorkMinX + 20.0f);
    click(ImVec2(current->Columns[1].WorkMinX + 20.0f, y));
    EXPECT_EQ(focused_, inspection_.selectedObjectIndex);
  }

  ImVec2 sunHoursCellPosition(double hours, double windowHours) {
    const auto color = container::renderer::BimSunHoursColor(hours, windowHours);
    const auto packed = ImGui::ColorConvertFloat4ToU32(
        ImVec4(color.r, color.g, color.b, 1.0f));
    ImVec2 low(std::numeric_limits<float>::max(), std::numeric_limits<float>::max());
    ImVec2 high(0.0f, 0.0f);
    int vertices = 0;
    for (const auto *list : ImGui::GetDrawData()->CmdLists) {
      for (const auto &vertex : list->VtxBuffer) {
        if (vertex.col == packed) {
          low.x = std::min(low.x, vertex.pos.x);
          low.y = std::min(low.y, vertex.pos.y);
          high.x = std::max(high.x, vertex.pos.x);
          high.y = std::max(high.y, vertex.pos.y);
          ++vertices;
        }
      }
    }
    EXPECT_EQ(vertices, 4);
    return ImVec2((low.x + high.x) * 0.5f, (low.y + high.y) * 0.5f);
  }

  container::ui::GuiManager gui_{};
  container::ui::BimInspectionState inspection_{};
  std::vector<container::renderer::BimElementMetadata> elements_{};
  ImGuiContext *previousContext_{};
  ImGuiContext *context_{};
  ImVec2 searchPosition_{};
  ImVec2 lastItemCenter_{};
  int firstModel_{}, secondModel_{};
  bool browserOpen_{true};
  bool drawSunHours_{};
  bool drawRenderSetups_{};
  ImGuiID renderSetupsId_{};
  uint32_t selected_{std::numeric_limits<uint32_t>::max()};
  uint32_t focused_{std::numeric_limits<uint32_t>::max()};
};

TEST_F(BimElementBrowserWidgetTests,
       FinalRowPastTenThousandSelectsAndFocusesObject) {
  // Imported metadata may contain line breaks. Fixed-height clipping must
  // retain final-row hit rectangles while tooltips expose the original text.
  for (size_t index = 0; index < elements_.size(); index += 1'000u) {
    auto &element = elements_[index];
    element.guid += "\r\nAdditional identity";
    element.sourceId += "\nProvider detail";
    element.type += "\tType detail";
    element.displayName += "\nDescription";
    element.storeyName += "\r\nStorey detail";
    element.materialName += "\nMaterial detail";
  }
  settle();
  auto *current = table();
  ASSERT_NE(current, nullptr);
  ASSERT_NE(current->InnerWindow, nullptr);
  EXPECT_LE(current->InnerWindow->Size.y, 300.0f);
  EXPECT_GT(current->InnerWindow->ScrollMax.y, 100'000.0f);
  EXPECT_GT(current->InnerWindow->ScrollMax.x, 0.0f);
  EXPECT_EQ(current->FreezeRowsRequest, 1);
  EXPECT_EQ(current->FreezeColumnsRequest, 2);
  ImGui::SetScrollY(current->InnerWindow, current->InnerWindow->ScrollMax.y);
  settle();
  EXPECT_NEAR(current->InnerWindow->Scroll.y, current->InnerWindow->ScrollMax.y,
              1.0f);
  selectLastVisibleRow();
  EXPECT_EQ(selected_, elements_.back().objectIndex);
  focusLastVisibleRow();
  EXPECT_EQ(focused_, elements_.back().objectIndex);
}

TEST_F(BimElementBrowserWidgetTests,
       ActualInputFiltersWithoutLosingObjectIdentity) {
  settle();
  typeQuery("FINALONLY");
  auto *current = table();
  ASSERT_NE(current, nullptr);
  EXPECT_EQ(current->InnerWindow->ScrollMax.y, 0.0f);
  EXPECT_EQ(current->CurrentRow, 1);
  const auto metadataBuilds = gui_.bimElementSearchCache().metadataBuildCount();
  const auto queryBuilds = gui_.bimElementSearchCache().matchBuildCount();
  for (size_t frameIndex = 0; frameIndex < 30; ++frameIndex)
    frame();
  EXPECT_EQ(gui_.bimElementSearchCache().metadataBuildCount(), metadataBuilds);
  EXPECT_EQ(gui_.bimElementSearchCache().matchBuildCount(), queryBuilds);

  typeQuery("finalonly");
  EXPECT_EQ(gui_.bimElementSearchCache().matchBuildCount(), queryBuilds);
  selectLastVisibleRow();
  EXPECT_EQ(selected_, elements_.back().objectIndex);
  focusLastVisibleRow();
  EXPECT_EQ(focused_, elements_.back().objectIndex);

  typeQuery("No such building element");
  ASSERT_NE(table(), nullptr);
  EXPECT_EQ(table()->CurrentRow, 0);
  EXPECT_EQ(table()->InnerWindow->ScrollMax.y, 0.0f);
  typeQuery("");
  EXPECT_GT(table()->InnerWindow->ScrollMax.y, 100'000.0f);
}

TEST_F(BimElementBrowserWidgetTests,
       InPlacePublicationAndSourceReplacementRefreshQuery) {
  settle();
  typeQuery("FinalOnly");
  ASSERT_EQ(table()->CurrentRow, 1);
  const auto metadataBuilds = gui_.bimElementSearchCache().metadataBuildCount();
  elements_.back().displayName = "Unique ReplacementOnly element";
  elements_.back().objectIndex = 123'456u;
  ++inspection_.metadataRevision;
  settle();
  EXPECT_EQ(table()->CurrentRow, 0);
  EXPECT_EQ(gui_.bimElementSearchCache().metadataBuildCount(),
            metadataBuilds + 1u);
  typeQuery("replacementonly");
  selectLastVisibleRow();
  EXPECT_EQ(selected_, 123'456u);

  // A different source can reuse both metadata storage and revision values.
  inspection_.metadataSource = &secondModel_;
  elements_.back().displayName = "Unique SourceOnly element";
  elements_.back().objectIndex = 654'321u;
  settle();
  EXPECT_EQ(table()->CurrentRow, 0);
  typeQuery("sourceonly");
  selectLastVisibleRow();
  EXPECT_EQ(selected_, 654'321u);
  focusLastVisibleRow();
  EXPECT_EQ(focused_, 654'321u);
}

TEST_F(BimElementBrowserWidgetTests,
       UnloadAndReloadWithReusedIdentityDiscardSearchData) {
  settle();
  typeQuery("FinalOnly");
  const auto metadataBuilds = gui_.bimElementSearchCache().metadataBuildCount();
  inspection_.hasScene = false;
  settle();
  EXPECT_EQ(gui_.bimElementSearchCache().metadataBuildCount(), metadataBuilds);
  elements_.back().displayName = "Replacement after unloading";
  inspection_.hasScene = true;
  // Deliberately retain source, pointer, count and revision to verify clear.
  settle();
  ASSERT_NE(table(), nullptr);
  EXPECT_EQ(table()->CurrentRow, 0);
  EXPECT_EQ(gui_.bimElementSearchCache().metadataBuildCount(),
            metadataBuilds + 1u);
  typeQuery("replacement after unloading");
  selectLastVisibleRow();
  EXPECT_EQ(selected_, elements_.back().objectIndex);
}

TEST_F(BimElementBrowserWidgetTests,
       ClosedBrowserReleasesRetiredSearchDataWithoutBuildingReplacement) {
  // The widget can identify read-only metadata fixtures by their view address.
  inspection_.metadataSource = nullptr;
  settle();
  typeQuery("FinalOnly");
  const auto &cache = gui_.bimElementSearchCache();
  const auto retainedBytes = cache.estimatedBytes();
  const auto emptyBytes = container::ui::BimElementSearchCache{}.estimatedBytes();
  const auto metadataBuilds = cache.metadataBuildCount();
  const auto matchBuilds = cache.matchBuildCount();
  ASSERT_GT(retainedBytes, emptyBytes);

  browserOpen_ = false;
  settle();
  EXPECT_EQ(cache.estimatedBytes(), retainedBytes);
  elements_.back().displayName = "Replacement after closing";
  ++inspection_.metadataRevision;
  settle();
  EXPECT_EQ(cache.estimatedBytes(), emptyBytes);
  EXPECT_EQ(cache.metadataBuildCount(), metadataBuilds);
  EXPECT_EQ(cache.matchBuildCount(), matchBuilds);

  browserOpen_ = true;
  settle();
  ASSERT_NE(table(), nullptr);
  EXPECT_EQ(table()->CurrentRow, 0);
  EXPECT_EQ(cache.metadataBuildCount(), metadataBuilds + 1u);
  EXPECT_EQ(cache.matchBuildCount(), matchBuilds + 1u);
  typeQuery("replacement after closing");
  selectLastVisibleRow();
  EXPECT_EQ(selected_, elements_.back().objectIndex);

  const auto reopenedMetadataBuilds = cache.metadataBuildCount();
  const auto reopenedMatchBuilds = cache.matchBuildCount();
  browserOpen_ = false;
  inspection_.hasScene = false;
  settle();
  EXPECT_EQ(cache.estimatedBytes(), emptyBytes);
  EXPECT_EQ(cache.metadataBuildCount(), reopenedMetadataBuilds);
  EXPECT_EQ(cache.matchBuildCount(), reopenedMatchBuilds);
}

TEST_F(BimElementBrowserWidgetTests, InspectionResetClearsAnActiveSearchInput) {
  settle();
  typeQuery("FinalOnly");
  ASSERT_EQ(table()->CurrentRow, 1);
  gui_.resetModelInspectionState();
  settle();
  ASSERT_NE(table(), nullptr);
  EXPECT_GT(table()->InnerWindow->ScrollMax.y, 100'000.0f);
}

TEST_F(BimElementBrowserWidgetTests,
       CompactPublicationFiltersSelectsAndFocusesAfterImportDataIsReleased) {
  const uint32_t finalObject = elements_.back().objectIndex;
  container::renderer::BimMetadataStorage storage;
  storage.replace(elements_);
  inspection_.metadataSource = &storage;
  inspection_.elements = storage.view();
  elements_.clear();
  elements_.shrink_to_fit();
  ASSERT_TRUE(elements_.empty());
  ASSERT_EQ(inspection_.elements.size(), 10'001u);

  settle();
  ASSERT_NE(table(), nullptr);
  EXPECT_GT(table()->InnerWindow->ScrollMax.y, 100'000.0f);
  typeQuery("FINALONLY");
  ASSERT_NE(table(), nullptr);
  ASSERT_EQ(table()->CurrentRow, 1);
  EXPECT_EQ(table()->InnerWindow->ScrollMax.y, 0.0f);
  const auto metadataBuilds = gui_.bimElementSearchCache().metadataBuildCount();
  const auto queryBuilds = gui_.bimElementSearchCache().matchBuildCount();
  for (size_t index = 0; index < 10u; ++index)
    frame();
  EXPECT_EQ(gui_.bimElementSearchCache().metadataBuildCount(), metadataBuilds);
  EXPECT_EQ(gui_.bimElementSearchCache().matchBuildCount(), queryBuilds);

  selectLastVisibleRow();
  EXPECT_EQ(selected_, finalObject);
  focusLastVisibleRow();
  EXPECT_EQ(focused_, finalObject);
  EXPECT_EQ(inspection_.elements.back().type, "IfcWall");
  EXPECT_EQ(inspection_.elements.back().materialName, "Concrete");

  // Retire borrowed views before the compact model's arenas are released.
  inspection_.hasScene = false;
  inspection_.elements = {};
  settle();
  storage.clear();
  EXPECT_EQ(storage.estimatedBytes(), 0u);
}

TEST_F(BimElementBrowserWidgetTests,
       CompactReplacementRevisionAndUnloadResetRetireOldSearchData) {
  container::renderer::BimMetadataStorage storage;
  storage.replace(elements_);
  inspection_.metadataSource = &storage;
  inspection_.elements = storage.view();
  elements_.clear();
  elements_.shrink_to_fit();
  settle();
  typeQuery("FinalOnly");
  ASSERT_NE(table(), nullptr);
  ASSERT_EQ(table()->CurrentRow, 1);
  const auto metadataBuilds = gui_.bimElementSearchCache().metadataBuildCount();

  {
    std::vector<container::renderer::BimElementMetadata> replacement(2u);
    replacement[0].objectIndex = 123'455u;
    replacement[0].guid = "Replacement first identity";
    replacement[0].type = "IfcDoor";
    replacement[1].objectIndex = 123'456u;
    replacement[1].guid = "Replacement final identity";
    replacement[1].type = "IfcDoor";
    replacement[1].displayName = "Unique ReplacementOnly element";
    replacement[1].storeyName = "Replacement storey";
    replacement[1].materialName = "Replacement material";
    storage.replace(replacement);
  }
  inspection_.elements = storage.view();
  ++inspection_.metadataRevision;
  settle();
  ASSERT_NE(table(), nullptr);
  EXPECT_EQ(table()->CurrentRow, 0);
  EXPECT_EQ(gui_.bimElementSearchCache().metadataBuildCount(), metadataBuilds + 1u);
  typeQuery("replacementonly");
  ASSERT_EQ(table()->CurrentRow, 1);
  selectLastVisibleRow();
  EXPECT_EQ(selected_, 123'456u);
  settle();
  focusLastVisibleRow();
  EXPECT_EQ(focused_, 123'456u);

  // The active text input must also forget its old query during reset.
  gui_.resetModelInspectionState();
  settle();
  ASSERT_NE(table(), nullptr);
  EXPECT_EQ(table()->CurrentRow, 2);
  typeQuery("replacementonly");
  ASSERT_EQ(table()->CurrentRow, 1);
  const auto beforeUnloadBytes = gui_.bimElementSearchCache().estimatedBytes();
  inspection_.hasScene = false;
  inspection_.elements = {};
  settle();
  storage.clear();
  EXPECT_EQ(storage.estimatedBytes(), 0u);
  EXPECT_LT(gui_.bimElementSearchCache().estimatedBytes(), beforeUnloadBytes);
  gui_.resetModelInspectionState();
  settle();

  container::renderer::BimElementMetadata reloaded;
  reloaded.objectIndex = 654'321u;
  reloaded.guid = "Reloaded compact identity";
  reloaded.type = "IfcWindow";
  reloaded.displayName = "Unique ReloadOnly element";
  storage.push_back(std::move(reloaded));
  storage.seal();
  inspection_.hasScene = true;
  inspection_.elements = storage.view();
  ++inspection_.metadataRevision;
  settle();
  ASSERT_NE(table(), nullptr);
  EXPECT_EQ(table()->CurrentRow, 1);
  typeQuery("reloadonly");
  selectLastVisibleRow();
  EXPECT_EQ(selected_, 654'321u);
  focusLastVisibleRow();
  EXPECT_EQ(focused_, 654'321u);

  inspection_.hasScene = false;
  inspection_.elements = {};
  settle();
  storage.clear();
}

TEST_F(BimElementBrowserWidgetTests, OriginalSecondRowFocusDoesNotShareTheColumnHeaderId) {
  elements_[1].displayName = "HeaderCollisionRow";
  settle();
  typeQuery("HeaderCollisionRow");
  ASSERT_NE(table(), nullptr);
  ASSERT_EQ(table()->CurrentRow, 1);
  selectLastVisibleRow();
  EXPECT_EQ(selected_, elements_[1].objectIndex);
  focusLastVisibleRow();
  EXPECT_EQ(focused_, elements_[1].objectIndex);
}

TEST_F(BimElementBrowserWidgetTests, SunHoursRunsCancelsSelectsAndExports) {
  drawSunHours_ = true;
  ImGui::GetIO().DisplaySize = ImVec2(1920.0f, 1080.0f);
  inspection_.sunHoursModelBounds.valid = true;
  inspection_.sunHoursModelBounds.min = {-25.0f, 4.0f, -11.0f};
  inspection_.sunHoursModelBounds.max = {33.0f, 24.0f, 60.0f};
  settle();
  click(lastItemCenter_);
  auto request = gui_.consumeBimSunHoursRunRequest();
  ASSERT_TRUE(request);
  EXPECT_DOUBLE_EQ(request->minX, -25.0);
  EXPECT_DOUBLE_EQ(request->maxZ, 60.0);
  EXPECT_DOUBLE_EQ(request->elevation, 4.0);

  gui_.setBimSunHoursState({.running = true});
  settle();
  click(lastItemCenter_);
  EXPECT_TRUE(gui_.consumeBimSunHoursCancelRequest());
  EXPECT_FALSE(gui_.consumeBimSunHoursCancelRequest());

  auto result = std::make_shared<container::renderer::BimSunHoursResult>();
  result->settings = *request;
  result->settings.rows = 2;
  result->settings.columns = 2;
  result->hours = {1.23, 2.34, 3.45, 6.15};
  gui_.setBimSunHoursState({.result = result});
  settle();
  const ImVec2 bottomLeft = sunHoursCellPosition(1.23, 10.0);
  const ImVec2 topRight = sunHoursCellPosition(6.15, 10.0);
  EXPECT_LT(bottomLeft.x, topRight.x);
  EXPECT_GT(bottomLeft.y, topRight.y);
  EXPECT_FALSE(gui_.bimSunHoursSelectedCell());
  click(bottomLeft);
  EXPECT_EQ(gui_.bimSunHoursSelectedCell(), 0u);
  click(topRight);
  EXPECT_EQ(gui_.bimSunHoursSelectedCell(), 3u);
  click(lastItemCenter_);
  EXPECT_EQ(gui_.consumeBimSunHoursExportRequest(), "bim-sun-hours.csv");

  gui_.setBimSunHoursState({});
  EXPECT_FALSE(gui_.bimSunHoursSelectedCell());
}

TEST_F(BimElementBrowserWidgetTests, NamedRenderSetupPreviewAndSidecarActions) {
  using container::ui::RenderSetupAction;
  drawRenderSetups_ = true;
  gui_.setRenderSetupState({.available = true, .path = "looks.json"});
  settle();
  click(renderSetupControl("Setup name"));
  ImGui::GetIO().AddInputCharactersUTF8("Morning");
  frame();
  click(renderSetupControl("Capture current view"));
  auto capture = gui_.consumeRenderSetupRequest();
  ASSERT_TRUE(capture);
  EXPECT_EQ(capture->action, RenderSetupAction::Capture);
  EXPECT_EQ(capture->name, "Morning");

  gui_.setRenderSetupState({.available = true, .names = {"Morning", "Evening"},
                            .path = "looks.json"});
  settle();
  const auto updateButton = renderSetupControl("Update from current view", 125.0f);
  click(updateButton);
  auto update = gui_.consumeRenderSetupRequest();
  ASSERT_TRUE(update);
  EXPECT_EQ(update->action, RenderSetupAction::Update);
  EXPECT_EQ(update->name, "Morning");
  click(renderSetupControl("Activate"));
  auto firstPreview = gui_.consumeRenderSetupRequest();
  ASSERT_TRUE(firstPreview);
  EXPECT_EQ(firstPreview->action, RenderSetupAction::Activate);
  EXPECT_EQ(firstPreview->name, "Morning");

  gui_.setRenderSetupState({.available = true, .names = {"Morning", "Evening"},
                            .activeName = "Evening", .path = "looks.json"});
  settle();
  const auto activateButton = renderSetupControl("Activate");
  click(ImVec2(updateButton.x, activateButton.y));
  EXPECT_FALSE(gui_.consumeRenderSetupRequest());

  click(renderSetupControl("Deactivate preview"));
  auto deactivate = gui_.consumeRenderSetupRequest();
  ASSERT_TRUE(deactivate);
  EXPECT_EQ(deactivate->action, RenderSetupAction::Deactivate);

  gui_.setRenderSetupState({.available = true, .names = {"Morning", "Evening"},
                            .path = "looks.json"});
  settle();
  click(renderSetupControl("Open"));
  auto open = gui_.consumeRenderSetupRequest();
  ASSERT_TRUE(open);
  EXPECT_EQ(open->action, RenderSetupAction::Open);
  EXPECT_EQ(open->path, "looks.json");
  click(renderSetupControl("Save", 80.0f));
  auto save = gui_.consumeRenderSetupRequest();
  ASSERT_TRUE(save);
  EXPECT_EQ(save->action, RenderSetupAction::Save);
  EXPECT_EQ(save->path, "looks.json");
  EXPECT_FALSE(gui_.consumeRenderSetupRequest());
}

} // namespace
