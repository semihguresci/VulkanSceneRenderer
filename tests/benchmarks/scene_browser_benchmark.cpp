// Opt-in real-ImGui CPU benchmark, excluded from CTest. Compile this same
// source with pre-change headers AND libraries under
// CONTAINER_SCENE_BROWSER_BASELINE. Fixture creation, opening the hierarchy and
// typing the filter are unmeasured. Timings include NewFrame, the actual
// production widgets and Render; allocation counts run separately after warmup.
// No platform/Vulkan backend is required.
#include "Container/renderer/bim/BimManager.h"
#include "Container/utility/GuiManager.h"
#include "Container/utility/SceneGraph.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <new>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <malloc.h>
#endif

namespace allocation_tracking {
thread_local bool enabled = false;
thread_local std::uint64_t calls = 0;
thread_local std::uint64_t bytes = 0;

void record(std::size_t size) noexcept {
  if (enabled) {
    ++calls;
    bytes += size;
  }
}

void *allocate(std::size_t size) {
  if (void *memory = std::malloc(size == 0 ? 1 : size)) {
    record(size);
    return memory;
  }
  throw std::bad_alloc();
}

void *allocateAligned(std::size_t size, std::size_t alignment) {
#ifdef _WIN32
  void *memory = _aligned_malloc(size == 0 ? 1 : size, alignment);
#else
  void *memory = nullptr;
  if (posix_memalign(&memory, alignment, size == 0 ? 1 : size) != 0) {
    memory = nullptr;
  }
#endif
  if (!memory)
    throw std::bad_alloc();
  record(size);
  return memory;
}

void freeAligned(void *memory) noexcept {
#ifdef _WIN32
  _aligned_free(memory);
#else
  std::free(memory);
#endif
}
} // namespace allocation_tracking

void *operator new(std::size_t size) {
  return allocation_tracking::allocate(size);
}
void *operator new[](std::size_t size) {
  return allocation_tracking::allocate(size);
}
void operator delete(void *memory) noexcept { std::free(memory); }
void operator delete[](void *memory) noexcept { std::free(memory); }
void operator delete(void *memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void *memory, std::size_t) noexcept {
  std::free(memory);
}
void *operator new(std::size_t size, std::align_val_t alignment) {
  return allocation_tracking::allocateAligned(
      size, static_cast<std::size_t>(alignment));
}
void *operator new[](std::size_t size, std::align_val_t alignment) {
  return allocation_tracking::allocateAligned(
      size, static_cast<std::size_t>(alignment));
}
void operator delete(void *memory, std::align_val_t) noexcept {
  allocation_tracking::freeAligned(memory);
}
void operator delete[](void *memory, std::align_val_t) noexcept {
  allocation_tracking::freeAligned(memory);
}
void operator delete(void *memory, std::size_t, std::align_val_t) noexcept {
  allocation_tracking::freeAligned(memory);
}
void operator delete[](void *memory, std::size_t, std::align_val_t) noexcept {
  allocation_tracking::freeAligned(memory);
}
void *operator new(std::size_t size, const std::nothrow_t &) noexcept {
  try {
    return ::operator new(size);
  } catch (...) {
    return nullptr;
  }
}
void *operator new[](std::size_t size, const std::nothrow_t &) noexcept {
  try {
    return ::operator new[](size);
  } catch (...) {
    return nullptr;
  }
}
void operator delete(void *memory, const std::nothrow_t &) noexcept {
  std::free(memory);
}
void operator delete[](void *memory, const std::nothrow_t &) noexcept {
  std::free(memory);
}
void *operator new(std::size_t size, std::align_val_t alignment,
                   const std::nothrow_t &) noexcept {
  try {
    return ::operator new(size, alignment);
  } catch (...) {
    return nullptr;
  }
}
void *operator new[](std::size_t size, std::align_val_t alignment,
                     const std::nothrow_t &) noexcept {
  try {
    return ::operator new[](size, alignment);
  } catch (...) {
    return nullptr;
  }
}
void operator delete(void *memory, std::align_val_t,
                     const std::nothrow_t &) noexcept {
  allocation_tracking::freeAligned(memory);
}
void operator delete[](void *memory, std::align_val_t,
                       const std::nothrow_t &) noexcept {
  allocation_tracking::freeAligned(memory);
}

namespace imgui_allocation_tracking {
struct alignas(std::max_align_t) Header {
  std::size_t bytes;
};
bool enabled = false;
std::uint64_t calls = 0;
std::uint64_t bytes = 0;
std::uint64_t liveBytes = 0;

void *allocate(std::size_t size, void *) {
  auto *header = static_cast<Header *>(std::malloc(sizeof(Header) + size));
  if (header == nullptr)
    return nullptr;
  header->bytes = size;
  liveBytes += size;
  if (enabled) {
    ++calls;
    bytes += size;
  }
  return header + 1;
}
void release(void *memory, void *) {
  if (memory == nullptr)
    return;
  auto *header = static_cast<Header *>(memory) - 1;
  liveBytes -= header->bytes;
  std::free(header);
}
} // namespace imgui_allocation_tracking

namespace {
using Clock = std::chrono::steady_clock;
using container::scene::SceneGraph;
using container::ui::GuiManager;

struct Measurement {
  std::string_view name;
  std::size_t elements = 0;
  std::size_t iterations = 0;
  std::size_t allocationIterations = 0;
  std::array<double, 7> microsecondsPerFrame{};
  std::uint64_t cppAllocations = 0;
  std::uint64_t cppRequestedBytes = 0;
  std::uint64_t imguiAllocations = 0;
  std::uint64_t imguiRequestedBytes = 0;
  std::uint64_t imguiLiveBytes = 0;
  std::size_t cachedBytes = 0;
  std::uint64_t metadataBuilds = 0;
  std::uint64_t matchBuilds = 0;
  int vertices = 0;
  bool verificationRequested = false;
  bool validationPassed = false;
  bool rowInteractionApplicable = false;
  bool middleRowVerified = false;
  bool finalRowVerified = false;
  bool searchFieldsVerified = false;
  bool cacheReuseVerified = false;
  bool unloadResetVerified = false;
  std::size_t sourceCapacityBytes = 0;
  std::size_t runtimeMetadataRecordBytes = 0;
  std::size_t sharedMetadataStrings = 0;
  std::size_t internedSearchLabels = 0;
  bool usesInternedSearchLabels = false;
};

enum class FixtureProfile { Rich, Compact };

enum class WidgetCase {
  HierarchyExpanded,
  HierarchyCollapsed,
  HierarchyPanelClosed,
  BimAll,
  BimPanelClosed,
  BimFiltered
};

class Fixture {
public:
  Fixture(std::size_t count, WidgetCase widgetCase, FixtureProfile profile,
          bool verifyLarge)
      : widgetCase_(widgetCase), profile_(profile), count_(count),
        verificationRequested_(verifyLarge) {
    context_ = ImGui::CreateContext();
    auto &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(1100.0f, 700.0f);
    io.DeltaTime = 1.0f / 60.0f;
    unsigned char *pixels{};
    int width{}, height{};
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    if (isHierarchy()) {
      root_ = graph_.createNode(
          glm::mat4(1.0f), 0, false, SceneGraph::kInvalidNode,
          profile_ == FixtureProfile::Compact ? "Scene group"
                                              : "Benchmark scene group");
      for (std::size_t index = 1; index < count; ++index) {
        const auto child = graph_.createNode(
            glm::mat4(1.0f), 0, false, SceneGraph::kInvalidNode,
            (profile_ == FixtureProfile::Compact ? "Node "
                                                 : "Benchmark scene node ") +
                std::to_string(index));
        if (!graph_.setParent(child, root_))
          throw std::runtime_error("could not parent benchmark node");
      }
    } else {
#if defined(CONTAINER_SCENE_BROWSER_BASELINE) || defined(CONTAINER_SCENE_BROWSER_CURRENT_BASELINE)
      elements_.reserve(count);
#else
      compactElements_.reserve(count);
#endif
      for (std::size_t index = 0; index < count; ++index) {
        container::renderer::BimElementMetadata element;
        element.objectIndex = static_cast<uint32_t>(index);
        if (profile_ == FixtureProfile::Compact) {
          element.guid = "id-" + std::to_string(index);
          element.sourceId = "src-" + std::to_string(index);
          element.type = "IfcWall";
          element.displayName = "Wall";
          element.storeyName = "Level 01";
          element.materialName = "Concrete";
          if (index == 0u) setSentinel(element, "first");
          if (index == count / 2u) setSentinel(element, "middle");
          if (index == count - 1u) setSentinel(element, "final");
        } else {
          element.guid = "Benchmark element identity " + std::to_string(index);
          element.sourceId = "Provider source identity " + std::to_string(index);
          element.type = "IfcWallStandardCase";
          element.displayName = "Ordinary benchmark building wall";
          element.storeyName = "Benchmark building storey 01";
          element.materialName = "Cast in place reinforced concrete";
        }
        if (index == count - 1u)
          element.displayName = profile_ == FixtureProfile::Compact ? "LAST-MATCH" : "Only last-match";
#if defined(CONTAINER_SCENE_BROWSER_BASELINE) || defined(CONTAINER_SCENE_BROWSER_CURRENT_BASELINE)
        elements_.push_back(std::move(element));
#else
        compactElements_.push_back(std::move(element));
#endif
      }
#if !defined(CONTAINER_SCENE_BROWSER_BASELINE) && !defined(CONTAINER_SCENE_BROWSER_CURRENT_BASELINE)
      compactElements_.seal();
#endif
      inspection_.hasScene = true;
#if defined(CONTAINER_SCENE_BROWSER_BASELINE) || defined(CONTAINER_SCENE_BROWSER_CURRENT_BASELINE)
      inspection_.elements = elements_;
#else
      inspection_.elements = compactElements_.view();
#endif
#ifndef CONTAINER_SCENE_BROWSER_BASELINE
      inspection_.metadataSource = this;
      inspection_.metadataRevision = 1u;
#endif
      inspection_.selectObject = [this](uint32_t object) {
        selected_ = object;
        inspection_.hasSelection = true;
        inspection_.selectedObjectIndex = object;
      };
      inspection_.focusObject = [this](uint32_t object) { focused_ = object; };
    }
    frame();
    frame();
    if (widgetCase_ == WidgetCase::HierarchyExpanded) {
      auto *table = findTable();
      if (table == nullptr)
        throw std::runtime_error("hierarchy table absent");
      click(ImVec2(table->Columns[0].WorkMinX + ImGui::GetFontSize() * 0.5f,
                   (table->RowPosY1 + table->RowPosY2) * 0.5f));
      frame();
      frame();
      table = findTable();
      if (table == nullptr || table->InnerWindow->ScrollMax.y < 1000.0f)
        throw std::runtime_error("expanded hierarchy did not expose children");
    }
    if (widgetCase_ == WidgetCase::BimFiltered) {
      click(searchPosition_);
      io.AddInputCharactersUTF8("LAST-MATCH");
      frame();
      frame();
      auto *table = findTable();
      if (table == nullptr || table->InnerWindow->ScrollMax.y != 0.0f)
        throw std::runtime_error("BIM input filter did not reduce the table");
      // Deactivate the input before measurement; the fixed filter stays set.
      click(ImVec2(1040.0f, 610.0f));
    }
    if (verifyLarge)
      verify();
    estimateSourceCapacity();
    io.AddMousePosEvent(-1000.0f, -1000.0f);
  }

  ~Fixture() { ImGui::DestroyContext(context_); }

  void frame() {
    ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(1080.0f, 660.0f));
    ImGui::Begin(kWindowName, nullptr,
                 ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoResize);
    ImGui::SetNextItemOpen(widgetCase_ != WidgetCase::HierarchyPanelClosed &&
                               widgetCase_ != WidgetCase::BimPanelClosed,
                           ImGuiCond_Always);
    if (isHierarchy()) {
      gui_.drawSceneHierarchy(
          graph_, root_, selected_, [this](uint32_t node) { selected_ = node; },
          [this](uint32_t node) { focused_ = node; }, {}, {});
    } else {
      const auto origin = ImGui::GetCursorScreenPos();
      searchPosition_ =
          ImVec2(origin.x + ImGui::GetStyle().IndentSpacing + 40.0f,
                 origin.y + ImGui::GetTextLineHeightWithSpacing() +
                     ImGui::GetFrameHeight() * 0.5f);
      gui_.drawBimElementBrowser(inspection_);
    }
    ImGui::End();
    ImGui::Render();
    vertices_ = ImGui::GetDrawData()->TotalVtxCount;
  }

  void diagnostics(Measurement &result) const {
    result.vertices = vertices_;
    result.verificationRequested = verificationRequested_;
    result.validationPassed = validationPassed_;
    result.rowInteractionApplicable =
        widgetCase_ == WidgetCase::HierarchyExpanded ||
        widgetCase_ == WidgetCase::BimAll ||
        widgetCase_ == WidgetCase::BimFiltered;
    result.middleRowVerified = middleRowVerified_;
    result.finalRowVerified = finalRowVerified_;
    result.searchFieldsVerified = searchFieldsVerified_;
    result.cacheReuseVerified = cacheReuseVerified_;
    result.unloadResetVerified = unloadResetVerified_;
    result.sourceCapacityBytes = sourceCapacityBytes_;
#if !defined(CONTAINER_SCENE_BROWSER_BASELINE) && !defined(CONTAINER_SCENE_BROWSER_CURRENT_BASELINE)
    if (!isHierarchy()) {
      result.runtimeMetadataRecordBytes = container::renderer::BimMetadataStorage::recordSize();
      result.sharedMetadataStrings = compactElements_.sharedStringCount();
      result.internedSearchLabels = gui_.bimElementSearchCache().internedLabelCount();
      result.usesInternedSearchLabels = gui_.bimElementSearchCache().usesInternedLabels();
    }
#endif
#ifndef CONTAINER_SCENE_BROWSER_BASELINE
    if (isHierarchy()) {
      const auto statistics = gui_.sceneHierarchyStatistics();
      result.cachedBytes = statistics.cachedBytes;
      result.metadataBuilds = statistics.cacheRebuilds;
    } else {
      const auto &cache = gui_.bimElementSearchCache();
      result.cachedBytes = cache.estimatedBytes();
      result.metadataBuilds = cache.metadataBuildCount();
      result.matchBuilds = cache.matchBuildCount();
    }
#endif
  }

private:
  void require(bool condition, std::string_view message) const {
    if (!condition)
      throw std::runtime_error("verification at " + std::to_string(count_) +
                               " entities: " + std::string(message));
  }

  void setSentinel(container::renderer::BimElementMetadata &element, std::string_view suffix) {
    element.guid = "guid-" + std::string(suffix);
    element.sourceId = "source-" + std::string(suffix);
    element.type = "type-" + std::string(suffix);
    element.displayName = "name-" + std::string(suffix);
    element.storeyName = "storey-" + std::string(suffix);
    element.materialName = "material-" + std::string(suffix);
  }

  void settle() {
    frame();
    frame();
    frame();
  }

  void typeQuery(std::string_view query) {
    click(searchPosition_);
    auto &io = ImGui::GetIO();
    io.AddKeyEvent(ImGuiMod_Ctrl, true);
    io.AddKeyEvent(ImGuiKey_A, true);
    frame();
    io.AddKeyEvent(ImGuiKey_A, false);
    io.AddKeyEvent(ImGuiMod_Ctrl, false);
    frame();
    if (query.empty()) {
      io.AddKeyEvent(ImGuiKey_Backspace, true);
      frame();
      io.AddKeyEvent(ImGuiKey_Backspace, false);
    } else {
      const std::string text(query);
      io.AddInputCharactersUTF8(text.c_str());
    }
    settle();
    require(context_->InputTextState.TextA.Data != nullptr &&
                std::string_view(context_->InputTextState.TextA.Data) == query,
            "real input events did not install the requested BIM query");
  }

  void clickRow(float y, uint32_t expected, std::string_view location) {
    auto *table = findTable();
    require(table != nullptr, "row interaction table is absent");
    require(
        y > table->BgClipRect.Min.y && y < table->InnerWindow->InnerRect.Max.y,
        std::string(location) +
            " row hit rectangle is outside viewport; y=" + std::to_string(y) +
            ", viewport=" + std::to_string(table->BgClipRect.Min.y) + ".." +
            std::to_string(table->InnerWindow->InnerRect.Max.y));
    selected_ = SceneGraph::kInvalidNode;
    focused_ = SceneGraph::kInvalidNode;
    click(ImVec2(table->Columns[0].WorkMinX + (isHierarchy() ? 65.0f : 40.0f),
                 y));
    require(selected_ == expected,
            std::string(location) + " selection expected original ID " +
                std::to_string(expected) + ", received " +
                std::to_string(selected_));
    table = findTable();
    require(table != nullptr, "focus interaction table is absent");
    click(ImVec2(table->Columns[isHierarchy() ? 2 : 1].WorkMinX + 20.0f, y));
    require(focused_ == expected, std::string(location) +
                                      " focus expected original ID " +
                                      std::to_string(expected) + ", received " +
                                      std::to_string(focused_));
  }

  void verifyMiddleAndFinalRows() {
    auto *table = findTable();
    require(table != nullptr, "scroll verification table is absent");
    const float rowHeight =
        ImGui::GetFrameHeight() + ImGui::GetStyle().CellPadding.y * 2.0f;
    const std::size_t middle = count_ / 2u;
    ImGui::SetScrollY(
        table->InnerWindow,
        static_cast<float>(static_cast<double>(middle) * rowHeight -
                           table->InnerWindow->InnerRect.GetHeight() * 0.5));
    settle();
    table = findTable();
    const double startY =
        static_cast<double>(table->InnerWindow->DC.CursorStartPos.y) +
        table->InnerWindow->DC.CursorStartPosLossyness.y;
    const double headerHeight =
        ImGui::GetTextLineHeight() + ImGui::GetStyle().CellPadding.y * 2.0f;
    const float middleY = static_cast<float>(
        startY + headerHeight + static_cast<double>(middle) * rowHeight +
        rowHeight * 0.5);
    const uint32_t middleId = isHierarchy() ? static_cast<uint32_t>(middle)
                                            : elementAt(middle).objectIndex;
    clickRow(middleY, middleId, "middle");
    middleRowVerified_ = true;

    table = findTable();
    ImGui::SetScrollY(table->InnerWindow, table->InnerWindow->ScrollMax.y);
    settle();
    table = findTable();
    const float finalY = (table->RowPosY1 + table->RowPosY2) * 0.5f;
    const uint32_t finalId = isHierarchy() ? static_cast<uint32_t>(count_ - 1u)
                                           : elementAt(count_ - 1u).objectIndex;
    clickRow(finalY, finalId, "final");
    finalRowVerified_ = true;
    table = findTable();
    ImGui::SetScrollY(table->InnerWindow, 0.0f);
    settle();
  }

  void verifySearchFields() {
    const auto &last = elementAt(count_ - 1u);
    const std::array<std::string, 6> queries{
        std::string(last.guid),        std::string(last.sourceId),   std::string(last.type),
        std::string(last.displayName), std::string(last.storeyName), std::string(last.materialName)};
    for (std::size_t field = 0; field < queries.size(); ++field) {
      std::string query = queries[field];
      std::ranges::transform(query, query.begin(), [](unsigned char value) {
        return static_cast<char>(std::toupper(value));
      });
      typeQuery(query);
#ifndef CONTAINER_SCENE_BROWSER_BASELINE
      const auto matches = existingMatches(query);
      const std::size_t expected =
          profile_ == FixtureProfile::Rich &&
                  (field == 2u || field == 4u || field == 5u)
              ? count_
              : 1u;
      require(matches.size() == expected && matches.back() == count_ - 1u,
              "BIM field " + std::to_string(field) +
                  " search lost the final original identity");
#endif
    }
    if (profile_ == FixtureProfile::Compact) {
      for (const std::size_t index :
           std::array<std::size_t, 2>{0u, count_ / 2u}) {
        typeQuery(elementAt(index).displayName);
#ifndef CONTAINER_SCENE_BROWSER_BASELINE
        const auto matches = existingMatches(elementAt(index).displayName);
        require(matches.size() == 1u && matches.front() == index,
                "first/middle BIM sentinel search returned the wrong row");
#endif
        auto *table = findTable();
        require(table != nullptr, "sentinel result table is absent");
        clickRow((table->RowPosY1 + table->RowPosY2) * 0.5f,
                 elementAt(index).objectIndex, "filtered sentinel");
      }
    }
    typeQuery("ABSENT-SCALE-ENTITY");
#ifndef CONTAINER_SCENE_BROWSER_BASELINE
    require(existingMatches("ABSENT-SCALE-ENTITY").empty(),
            "no-match BIM query retained old results");
#endif
    typeQuery("");
#ifndef CONTAINER_SCENE_BROWSER_BASELINE
    const auto all = existingMatches("");
    require(all.size() == count_ && all.front() == 0u &&
                all.back() == count_ - 1u,
            "empty BIM query did not restore all original rows");
#endif
    searchFieldsVerified_ = true;
  }

  void verifyCacheReuse() {
#ifndef CONTAINER_SCENE_BROWSER_BASELINE
    const auto hierarchyBuilds = gui_.sceneHierarchyStatistics().cacheRebuilds;
    const auto metadataBuilds =
        gui_.bimElementSearchCache().metadataBuildCount();
    const auto queryBuilds = gui_.bimElementSearchCache().matchBuildCount();
    for (std::size_t index = 0; index < 30u; ++index)
      frame();
    require(gui_.sceneHierarchyStatistics().cacheRebuilds == hierarchyBuilds &&
                gui_.bimElementSearchCache().metadataBuildCount() ==
                    metadataBuilds &&
                gui_.bimElementSearchCache().matchBuildCount() == queryBuilds,
            "unchanged widget frames rebuilt hierarchy or BIM search caches");
    cacheReuseVerified_ = true;
#endif
  }

#ifndef CONTAINER_SCENE_BROWSER_BASELINE
  std::span<const std::size_t> existingMatches(std::string_view query) {
    // GuiManager exposes read-only diagnostics. This fixture owns a mutable
    // manager; repeating its already installed query must leave both build
    // counters unchanged. Never use this helper to install a search query.
    const auto &diagnostics = gui_.bimElementSearchCache();
    const auto metadataBuilds = diagnostics.metadataBuildCount();
    const auto queryBuilds = diagnostics.matchBuildCount();
    const auto matches =
        const_cast<container::ui::BimElementSearchCache &>(diagnostics)
            .matches(query);
    require(diagnostics.metadataBuildCount() == metadataBuilds &&
                diagnostics.matchBuildCount() == queryBuilds,
            "BIM widget did not already cache the typed search query");
    return matches;
  }
#endif

  void verify() {
    require(isHierarchy() ? graph_.nodeCount() == count_
                          : inspection_.elements.size() == count_,
            "source fixture does not contain the requested entity count");
    if (widgetCase_ == WidgetCase::HierarchyExpanded ||
        widgetCase_ == WidgetCase::BimAll)
      verifyMiddleAndFinalRows();
    if (widgetCase_ == WidgetCase::BimAll) {
      verifySearchFields();
      verifyCacheReuse();
#ifndef CONTAINER_SCENE_BROWSER_BASELINE
      const auto previousBuilds =
          gui_.bimElementSearchCache().metadataBuildCount();
      inspection_.hasScene = false;
      settle();
      const container::ui::BimElementSearchCache empty;
      require(gui_.bimElementSearchCache().estimatedBytes() ==
                  empty.estimatedBytes(),
              "unloading did not release the large BIM search cache");
      inspection_.hasScene = true;
      settle();
      require(gui_.bimElementSearchCache().metadataBuildCount() ==
                  previousBuilds + 1u,
              "reloading did not rebuild the retired BIM search cache");
      gui_.resetModelInspectionState();
      settle();
      require(existingMatches("").size() == count_,
              "inspection reset lost BIM source rows");
      unloadResetVerified_ = true;
#endif
    } else if (widgetCase_ == WidgetCase::BimFiltered) {
#ifndef CONTAINER_SCENE_BROWSER_BASELINE
      const auto matches = existingMatches("LAST-MATCH");
      require(matches.size() == 1u && matches.front() == count_ - 1u,
              "filtered timing fixture is not the final entity alone");
#endif
      auto *table = findTable();
      require(table != nullptr, "filtered verification table is absent");
      clickRow((table->RowPosY1 + table->RowPosY2) * 0.5f,
               elementAt(count_ - 1u).objectIndex, "filtered final");
      finalRowVerified_ = true;
      verifyCacheReuse();
    } else {
      verifyCacheReuse();
    }
    // Restore the original timing interaction state after all correctness work.
    inspection_.hasSelection = false;
    selected_ = SceneGraph::kInvalidNode;
    focused_ = SceneGraph::kInvalidNode;
    click(ImVec2(1040.0f, 610.0f));
    validationPassed_ = true;
  }

  static std::size_t externalStringBytes(const std::string &value) {
    const auto object = reinterpret_cast<std::uintptr_t>(&value);
    const auto characters = reinterpret_cast<std::uintptr_t>(value.data());
    return characters >= object && characters < object + sizeof(value)
               ? 0u
               : value.capacity() + 1u;
  }

  void estimateSourceCapacity() {
    if (isHierarchy()) {
      // SceneGraph does not expose its node-vector capacity. Count live node
      // payloads and owned child/name capacities, excluding spare node
      // capacity.
      sourceCapacityBytes_ =
          graph_.nodeCount() * sizeof(container::scene::SceneNode) +
          graph_.rootNodes().capacity() * sizeof(uint32_t);
      for (std::size_t index = 0; index < graph_.nodeCount(); ++index) {
        const auto &node = *graph_.getNode(static_cast<uint32_t>(index));
        sourceCapacityBytes_ += externalStringBytes(node.name) +
                                node.children.capacity() * sizeof(uint32_t);
      }
    } else {
#if defined(CONTAINER_SCENE_BROWSER_BASELINE) || defined(CONTAINER_SCENE_BROWSER_CURRENT_BASELINE)
      sourceCapacityBytes_ = elements_.capacity() *
                             sizeof(container::renderer::BimElementMetadata);
      for (const auto &element : elements_)
        for (const std::string *field :
             {&element.guid, &element.sourceId, &element.type,
              &element.displayName, &element.storeyName, &element.materialName})
          sourceCapacityBytes_ += externalStringBytes(*field);
#else
      sourceCapacityBytes_ = compactElements_.estimatedBytes();
#endif
    }
  }

  decltype(auto) elementAt(std::size_t index) const {
#if defined(CONTAINER_SCENE_BROWSER_BASELINE) || defined(CONTAINER_SCENE_BROWSER_CURRENT_BASELINE)
    return (elements_[index]);
#else
    return compactElements_[index];
#endif
  }

  bool isHierarchy() const {
    return widgetCase_ == WidgetCase::HierarchyExpanded ||
           widgetCase_ == WidgetCase::HierarchyCollapsed ||
           widgetCase_ == WidgetCase::HierarchyPanelClosed;
  }
  static constexpr char kWindowName[] = "Scene browser benchmark";

  ImGuiTable *findTable() const {
    const auto *window = ImGui::FindWindowByName(kWindowName);
    if (!window) return nullptr;
    // The table is scoped by its production tree node. Use the same ID across
    // revisions where the hierarchy gained additional columns.
    const char *treeName = isHierarchy() ? "Scene Hierarchy" : "BIM Elements";
    const char *tableName = isHierarchy() ? "SceneHierarchyTable" : "BimElementsTable";
    const auto treeId = ImHashStr(treeName, 0, window->ID);
    auto *table = ImGui::TableFindByID(ImHashStr(tableName, 0, treeId));
    return table && table->LastFrameActive == ImGui::GetFrameCount() ? table : nullptr;
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

  WidgetCase widgetCase_;
  FixtureProfile profile_;
  std::size_t count_;
  SceneGraph graph_;
  GuiManager gui_;
#if defined(CONTAINER_SCENE_BROWSER_BASELINE) || defined(CONTAINER_SCENE_BROWSER_CURRENT_BASELINE)
  std::vector<container::renderer::BimElementMetadata> elements_;
#else
  container::renderer::BimMetadataStorage compactElements_;
#endif
  container::ui::BimInspectionState inspection_;
  ImGuiContext *context_{};
  uint32_t root_{SceneGraph::kInvalidNode};
  ImVec2 searchPosition_{};
  int vertices_{0};
  uint32_t selected_{SceneGraph::kInvalidNode};
  uint32_t focused_{SceneGraph::kInvalidNode};
  bool verificationRequested_{false};
  bool validationPassed_{false};
  bool middleRowVerified_{false};
  bool finalRowVerified_{false};
  bool searchFieldsVerified_{false};
  bool cacheReuseVerified_{false};
  bool unloadResetVerified_{false};
  std::size_t sourceCapacityBytes_{0};
};

Measurement measure(std::string_view name, std::size_t count,
                    WidgetCase widgetCase, FixtureProfile profile,
                    bool verifyLarge) {
  Fixture fixture(count, widgetCase, profile, verifyLarge);
  for (std::size_t index = 0; index < 24; ++index)
    fixture.frame();
  const auto calibrationStart = Clock::now();
  for (std::size_t index = 0; index < 4; ++index)
    fixture.frame();
  const double calibrationUs =
      std::chrono::duration<double, std::micro>(Clock::now() - calibrationStart)
          .count() /
      4.0;
  Measurement result{.name = name, .elements = count};
  result.iterations = static_cast<std::size_t>(std::clamp(
      std::ceil(10'000.0 / std::max(calibrationUs, 0.001)), 4.0, 4096.0));
  for (auto &sample : result.microsecondsPerFrame) {
    const auto start = Clock::now();
    for (std::size_t index = 0; index < result.iterations; ++index)
      fixture.frame();
    sample = std::chrono::duration<double, std::micro>(Clock::now() - start)
                 .count() /
             static_cast<double>(result.iterations);
  }
  result.allocationIterations = std::min<std::size_t>(result.iterations, 64u);
  allocation_tracking::calls = 0;
  allocation_tracking::bytes = 0;
  imgui_allocation_tracking::calls = 0;
  imgui_allocation_tracking::bytes = 0;
  allocation_tracking::enabled = true;
  imgui_allocation_tracking::enabled = true;
  for (std::size_t index = 0; index < result.allocationIterations; ++index)
    fixture.frame();
  allocation_tracking::enabled = false;
  imgui_allocation_tracking::enabled = false;
  result.cppAllocations = allocation_tracking::calls;
  result.cppRequestedBytes = allocation_tracking::bytes;
  result.imguiAllocations = imgui_allocation_tracking::calls;
  result.imguiRequestedBytes = imgui_allocation_tracking::bytes;
  result.imguiLiveBytes = imgui_allocation_tracking::liveBytes;
  fixture.diagnostics(result);
  return result;
}

void print(const Measurement &result) {
  auto ordered = result.microsecondsPerFrame;
  std::ranges::sort(ordered);
  const double allocations = static_cast<double>(result.allocationIterations);
  std::cout << "    {\"name\":\"" << result.name
            << "\",\"elements\":" << result.elements
            << ",\"iterations_per_repeat\":" << result.iterations
            << ",\"microseconds_per_frame\":[";
  for (std::size_t index = 0; index < result.microsecondsPerFrame.size();
       ++index) {
    if (index != 0)
      std::cout << ',';
    std::cout << result.microsecondsPerFrame[index];
  }
  std::cout << "],\"median_microseconds_per_frame\":" << ordered[3]
            << ",\"allocation_iterations\":" << result.allocationIterations
            << ",\"cpp_allocations_per_frame\":"
            << result.cppAllocations / allocations
            << ",\"cpp_requested_bytes_per_frame\":"
            << result.cppRequestedBytes / allocations
            << ",\"imgui_allocations_per_frame\":"
            << result.imguiAllocations / allocations
            << ",\"imgui_requested_bytes_per_frame\":"
            << result.imguiRequestedBytes / allocations
            << ",\"imgui_live_requested_bytes\":" << result.imguiLiveBytes
            << ",\"cache_capacity_estimate_bytes\":" << result.cachedBytes
            << ",\"metadata_builds\":" << result.metadataBuilds
            << ",\"query_builds\":" << result.matchBuilds
            << ",\"verification_requested\":" << result.verificationRequested
            << ",\"validation_passed\":" << result.validationPassed
            << ",\"row_interaction_applicable\":"
            << result.rowInteractionApplicable
            << ",\"middle_row_verified\":" << result.middleRowVerified
            << ",\"final_row_verified\":" << result.finalRowVerified
            << ",\"search_fields_verified\":" << result.searchFieldsVerified
            << ",\"cache_reuse_verified\":" << result.cacheReuseVerified
            << ",\"unload_reset_verified\":" << result.unloadResetVerified
            << ",\"source_payload_estimate_bytes\":"
            << result.sourceCapacityBytes
            << ",\"runtime_metadata_record_bytes\":" << result.runtimeMetadataRecordBytes
            << ",\"shared_metadata_strings\":" << result.sharedMetadataStrings
            << ",\"interned_search_labels\":" << result.internedSearchLabels
            << ",\"uses_interned_search_labels\":" << result.usesInternedSearchLabels
            << ",\"render_vertices\":" << result.vertices << '}';
}
} // namespace

int main(int argc, char **argv) {
  try {
    std::size_t count = 10'001u;
    bool countSupplied = false;
    bool verifyLarge = false;
    bool describe = false;
    FixtureProfile profile = FixtureProfile::Rich;
    std::string selectedCase;
    for (int index = 1; index < argc; ++index) {
      const std::string_view argument(argv[index]);
      if (argument == "--verify-large") {
        verifyLarge = true;
      } else if (argument == "--describe") {
        describe = true;
      } else if (argument == "--case" || argument == "--fixture") {
        if (++index >= argc)
          throw std::runtime_error(std::string(argument) + " needs a value");
        const std::string_view value(argv[index]);
        if (argument == "--case") {
          selectedCase = value;
        } else if (value == "rich") {
          profile = FixtureProfile::Rich;
        } else if (value == "compact") {
          profile = FixtureProfile::Compact;
        } else {
          throw std::runtime_error("--fixture must be rich or compact");
        }
      } else if (!countSupplied && !argument.starts_with('-')) {
        std::size_t consumed = 0u;
        const auto parsed = std::stoull(std::string(argument), &consumed);
        if (consumed != argument.size() || parsed > 10'000'000u)
          throw std::runtime_error("invalid entity count");
        count = static_cast<std::size_t>(parsed);
        countSupplied = true;
      } else {
        throw std::runtime_error("unexpected argument: " +
                                 std::string(argument));
      }
    }
    if (count < 100u || count > 10'000'000u)
      throw std::runtime_error("row count must be between 100 and 10000000");
    if (describe) {
      std::cout << "{\"schema_version\":2,\"description_only\":true,"
                   "\"sizeof_scene_node\":"
                << sizeof(container::scene::SceneNode)
                << ",\"sizeof_bim_element_metadata\":"
                << sizeof(container::renderer::BimElementMetadata)
#ifndef CONTAINER_SCENE_BROWSER_BASELINE
                << ",\"sizeof_hierarchy_row\":"
                << sizeof(container::ui::SceneHierarchyRow)
#endif
                << ",\"sizeof_string\":" << sizeof(std::string)
                << ",\"sizeof_index\":" << sizeof(std::size_t)
                << ",\"search_field_strings_per_entity\":6,"
                   "\"maximum_entities\":10000000}\n";
      return 0;
    }
    const std::array<std::pair<std::string_view, WidgetCase>, 6> cases{
        {{"hierarchy_expanded", WidgetCase::HierarchyExpanded},
         {"hierarchy_collapsed", WidgetCase::HierarchyCollapsed},
         {"hierarchy_panel_closed", WidgetCase::HierarchyPanelClosed},
         {"bim_empty_query", WidgetCase::BimAll},
         {"bim_panel_closed", WidgetCase::BimPanelClosed},
         {"bim_filtered_query", WidgetCase::BimFiltered}}};
    if (!selectedCase.empty() &&
        std::ranges::none_of(cases, [&](const auto &entry) {
          return entry.first == selectedCase;
        }))
      throw std::runtime_error("unknown --case: " + selectedCase);
    ImGuiMemAllocFunc previousAllocate{};
    ImGuiMemFreeFunc previousFree{};
    void *previousUserData{};
    ImGui::GetAllocatorFunctions(&previousAllocate, &previousFree,
                                 &previousUserData);
    ImGui::SetAllocatorFunctions(imgui_allocation_tracking::allocate,
                                 imgui_allocation_tracking::release);
    std::vector<Measurement> measurements;
    for (const auto &[name, widgetCase] : cases)
      if (selectedCase.empty() || selectedCase == name)
        measurements.push_back(
            measure(name, count, widgetCase, profile, verifyLarge));
    ImGui::SetAllocatorFunctions(previousAllocate, previousFree,
                                 previousUserData);
    std::cout
        << std::fixed << std::setprecision(3) << std::boolalpha
        << "{\n  \"schema_version\":2,\n  \"imgui_version\":\"" << IMGUI_VERSION
        << "\",\n  \"variant\":\""
#ifdef CONTAINER_SCENE_BROWSER_BASELINE
        << "before"
#elif defined(CONTAINER_SCENE_BROWSER_BEFORE_INTERNING)
        << "before_interning"
#elif defined(CONTAINER_SCENE_BROWSER_CURRENT_BASELINE)
        << "before_packed"
#else
        << "after"
#endif
        << "\",\n  \"scope\":\"full CPU ImGui frame without platform/GPU "
           "backend\","
        << "\n  \"fixture_profile\":\""
        << (profile == FixtureProfile::Compact ? "compact" : "rich")
        << "\",\n  \"sizeof_scene_node\":"
        << sizeof(container::scene::SceneNode)
        << ",\n  \"sizeof_bim_element_metadata\":"
        << sizeof(container::renderer::BimElementMetadata)
        << ",\n  \"sizeof_string\":" << sizeof(std::string)
        << ",\n  \"source_estimate_scope\":\"live node payloads; baseline owning metadata vector or current compact runtime record/text/dictionary capacities plus properties; "
           "excludes spare private node capacity and allocator bookkeeping\","
        << "\n  \"cases\":[\n";
    for (std::size_t index = 0; index < measurements.size(); ++index) {
      print(measurements[index]);
      std::cout << (index + 1 == measurements.size() ? "\n" : ",\n");
    }
    std::cout << "  ]\n}\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "scene browser benchmark failed: " << error.what() << '\n';
    return 1;
  }
}
