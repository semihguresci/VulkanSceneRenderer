#include "Container/utility/GuiManager.h"

#include "Container/renderer/bim/BimCoordinationOverlay.h"
#include "Container/renderer/bim/BimManager.h"
#include "Container/renderer/bim/BimMeasurementSnapping.h"
#include "Container/renderer/bim/BimSemanticColorMode.h"
#include "Container/renderer/core/RenderTechnique.h"
#include "Container/renderer/core/RendererTelemetry.h"
#include "Container/renderer/scene/ScenePrimitives.h"
#include "Container/utility/BcfViewpoint.h"

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

#include "Container/common/CommonGLFW.h"
#include "Container/common/CommonMath.h"
#include "Container/utility/Platform.h"
#include "Container/utility/SceneGraph.h"

namespace container::ui {

using container::gpu::kMaxDeferredPointLights;
using container::gpu::LightingData;
using container::gpu::LightingSettings;
using container::renderer::EditableLightEntity;
using container::renderer::EditableLightId;
using container::renderer::EditableLightSource;
using container::renderer::EditableLightType;
using container::renderer::ScenePrimitiveKind;

namespace {

constexpr std::string_view kSampleModelsRelativeRoot =
    "models/glTF-Sample-Models/2.0";

constexpr std::array<const char *, 3> kImportScaleLabels = {{
    "1x",
    "10x",
    "100x",
}};

constexpr std::array<float, 3> kImportScaleValues = {{
    1.0f,
    10.0f,
    100.0f,
}};

struct DisplayModeDefinition {
  GBufferViewMode mode;
  const char *label;
};

constexpr std::array<DisplayModeDefinition, 14> kDisplayModeDefinitions = {{
    {GBufferViewMode::Lit, "Lit"},
    {GBufferViewMode::Albedo, "Albedo"},
    {GBufferViewMode::Normals, "Normals"},
    {GBufferViewMode::Material, "Material"},
    {GBufferViewMode::Depth, "Depth"},
    {GBufferViewMode::Emissive, "Emissive"},
    {GBufferViewMode::Transparency, "Transparency"},
    {GBufferViewMode::Revealage, "Revealage"},
    {GBufferViewMode::Overview, "Overview"},
    {GBufferViewMode::SurfaceNormals, "Surface Normals"},
    {GBufferViewMode::ObjectSpaceNormals, "Object Normals"},
    {GBufferViewMode::ShadowCascades, "Shadow Cascades"},
    {GBufferViewMode::TileLightHeatMap, "Tile Light Heat Map"},
    {GBufferViewMode::ShadowTexelDensity, "Shadow Texel Density"},
}};

struct KnownSampleAsset {
  const char *label;
  const char *relativePath;
};

struct KnownSampleDirectory {
  const char *labelPrefix;
  const char *relativePath;
};

struct DiscoveredSampleAsset {
  std::string label;
  std::filesystem::path path;
};

constexpr std::array<KnownSampleAsset, 4> kKnownAuxiliarySampleAssets = {{
    {"USD / Kitchen Set",
     "models/OpenUSD-Sample-Assets/Kitchen_set/Kitchen_set.usd"},
    {"USD / Kitchen Set Instanced",
     "models/OpenUSD-Sample-Assets/Kitchen_set/Kitchen_set_instanced.usd"},
    {"USD / Point Instanced Med City",
     "models/OpenUSD-Sample-Assets/PointInstancedMedCity/"
     "PointInstancedMedCity.usd"},
    {"IFC / Tessellated Item",
     "models/buildingSMART-Sample-Test-Files/IFC 4.0.2.1 (IFC 4)/ISO Spec - "
     "ReferenceView_V1.2/tessellated-item.ifc"},
}};

constexpr std::array<KnownSampleDirectory, 2> kKnownAuxiliarySampleDirectories =
    {{
        {"IFC5", "models/buildingSMART-IFC5-development/examples"},
        {"IFC", "models/buildingSMART-Sample-Test-Files"},
    }};

std::string ToLowerAscii(std::string value) {
  std::ranges::transform(value, value.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return value;
}

std::string TrimAscii(std::string_view value) {
  size_t first = 0u;
  while (first < value.size() &&
         std::isspace(static_cast<unsigned char>(value[first])) != 0) {
    ++first;
  }
  size_t last = value.size();
  while (last > first &&
         std::isspace(static_cast<unsigned char>(value[last - 1u])) != 0) {
    --last;
  }
  return std::string(value.substr(first, last - first));
}

std::vector<std::string> SplitBcfTopicLabels(std::string_view value) {
  std::vector<std::string> labels;
  size_t start = 0u;
  for (size_t i = 0u; i <= value.size(); ++i) {
    if (i != value.size() && value[i] != ',' && value[i] != ';') {
      continue;
    }
    std::string label = TrimAscii(value.substr(start, i - start));
    if (!label.empty() && !std::ranges::contains(labels, label)) {
      labels.push_back(std::move(label));
    }
    start = i + 1u;
  }
  return labels;
}

std::string WrapLongPathForImGui(std::string_view text) {
  constexpr size_t kPathTooltipSoftBreakColumns = 32u;
  constexpr size_t kPathTooltipHardBreakColumns = 48u;

  std::string wrapped;
  wrapped.reserve(text.size() + text.size() / kPathTooltipSoftBreakColumns);
  size_t column = 0u;
  size_t unbrokenRun = 0u;
  for (size_t i = 0u; i < text.size(); ++i) {
    const char c = text[i];
    wrapped.push_back(c);

    if (c == '\n') {
      column = 0u;
      unbrokenRun = 0u;
      continue;
    }

    ++column;
    if (std::isspace(static_cast<unsigned char>(c)) != 0) {
      unbrokenRun = 0u;
      continue;
    }
    ++unbrokenRun;

    const bool pathSeparator = c == '\\' || c == '/';
    const bool softPathBreak =
        pathSeparator && column >= kPathTooltipSoftBreakColumns;
    const bool hardTokenBreak = unbrokenRun >= kPathTooltipHardBreakColumns;
    if ((softPathBreak || hardTokenBreak) && i + 1u < text.size()) {
      wrapped.push_back('\n');
      column = 0u;
      unbrokenRun = 0u;
    }
  }
  return wrapped;
}

void ShowWrappedTooltip(std::string_view text) {
  if (text.empty()) {
    return;
  }

  constexpr float kMaxWidthEm = 48.0f;
  constexpr float kMinWidthEm = 20.0f;
  const float fontSize = std::max(ImGui::GetFontSize(), 1.0f);
  const ImGuiStyle &style = ImGui::GetStyle();
  const float desiredWidth = fontSize * kMaxWidthEm;
  float viewportMaxWidth = desiredWidth;
  if (const ImGuiViewport *viewport = ImGui::GetMainViewport()) {
    viewportMaxWidth =
        std::max(fontSize, viewport->WorkSize.x * 0.45f -
                               style.WindowPadding.x * 2.0f);
  }
  const float minimumWidth = std::min(fontSize * kMinWidthEm, viewportMaxWidth);
  const float wrapWidth =
      std::clamp(desiredWidth, minimumWidth, viewportMaxWidth);

  ImGui::SetNextWindowSizeConstraints(
      ImVec2(0.0f, 0.0f),
      ImVec2(wrapWidth + style.WindowPadding.x * 2.0f,
             std::numeric_limits<float>::max()));
  const std::string tooltipText = WrapLongPathForImGui(text);
  ImGui::BeginTooltip();
  ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + wrapWidth);
  ImGui::TextUnformatted(tooltipText.data(),
                         tooltipText.data() + tooltipText.size());
  ImGui::PopTextWrapPos();
  ImGui::EndTooltip();
}

void ShowItemTooltip(std::string_view text, ImGuiHoveredFlags flags = 0) {
  if (!text.empty() && ImGui::IsItemHovered(flags)) {
    ShowWrappedTooltip(text);
  }
}

void TextWrappedPathValue(std::string_view label, std::string_view value) {
  const std::string wrappedValue = WrapLongPathForImGui(value);
  if (label.empty()) {
    ImGui::TextWrapped("%s", wrappedValue.c_str());
    return;
  }
  ImGui::TextWrapped("%.*s: %s", static_cast<int>(label.size()), label.data(),
                     wrappedValue.c_str());
}

std::string SceneHierarchyNodeName(
    const container::scene::SceneGraph &sceneGraph, uint32_t nodeIndex) {
  const auto *node = sceneGraph.getNode(nodeIndex);
  if (node == nullptr) {
    return "Invalid node";
  }

  std::string label =
      node->name.empty() ? "Node " + std::to_string(nodeIndex) : node->name;
  label += " [" + std::to_string(nodeIndex) + "]";
  if (node->primitiveIndex != container::scene::SceneGraph::kInvalidNode) {
    label += " / Primitive " + std::to_string(node->primitiveIndex);
  }
  return label;
}

std::vector<uint32_t> SceneHierarchyDescendants(
    const container::scene::SceneGraph &sceneGraph, uint32_t ancestor) {
  struct SceneHierarchyVisitedNodes {
    std::vector<uint8_t> visited;

    explicit SceneHierarchyVisitedNodes(size_t nodeCount)
        : visited(nodeCount, 0u) {}

    bool mark(uint32_t nodeIndex) {
      if (nodeIndex >= visited.size() || visited[nodeIndex] != 0u) {
        return false;
      }
      visited[nodeIndex] = 1u;
      return true;
    }
  };

  std::vector<uint32_t> descendants;
  const auto *ancestorNode = sceneGraph.getNode(ancestor);
  if (ancestorNode == nullptr) {
    return descendants;
  }

  SceneHierarchyVisitedNodes visited(sceneGraph.nodeCount());
  visited.mark(ancestor);
  std::vector<uint32_t> stack = ancestorNode->children;
  while (!stack.empty()) {
    const uint32_t current = stack.back();
    stack.pop_back();
    if (!visited.mark(current)) {
      continue;
    }
    descendants.push_back(current);
    if (const auto *node = sceneGraph.getNode(current)) {
      stack.insert(stack.end(), node->children.begin(), node->children.end());
    }
  }
  std::ranges::sort(descendants);
  descendants.erase(std::ranges::unique(descendants).begin(),
                    descendants.end());
  return descendants;
}

bool SceneHierarchyContainsNode(const std::vector<uint32_t> &sortedNodes,
                                uint32_t nodeIndex) {
  return std::ranges::binary_search(sortedNodes, nodeIndex);
}

void RefreshSceneHierarchyParentCandidateCache(
    const container::scene::SceneGraph &sceneGraph, uint32_t nodeIndex,
    uint32_t currentParent,
    SceneHierarchyParentCandidateCacheState &candidateCache) {
  if (candidateCache.revision == sceneGraph.revision() &&
      candidateCache.nodeCount == sceneGraph.nodeCount() &&
      candidateCache.rootCount == sceneGraph.rootNodes().size() &&
      candidateCache.nodeIndex == nodeIndex &&
      candidateCache.currentParent == currentParent) {
    return;
  }

  candidateCache.revision = sceneGraph.revision();
  candidateCache.nodeCount = sceneGraph.nodeCount();
  candidateCache.rootCount = sceneGraph.rootNodes().size();
  candidateCache.nodeIndex = nodeIndex;
  candidateCache.currentParent = currentParent;
  candidateCache.rows.clear();
  candidateCache.rows.push_back(SceneHierarchyParentCandidateRow{
      .parent = std::nullopt,
      .label = "None",
      .selected =
          currentParent == container::scene::SceneGraph::kInvalidNode});

  const std::vector<uint32_t> descendants =
      SceneHierarchyDescendants(sceneGraph, nodeIndex);
  const size_t candidateLimit =
      std::min(sceneGraph.nodeCount(),
               static_cast<size_t>(std::numeric_limits<uint32_t>::max()));
  candidateCache.rows.reserve(candidateLimit + 1u);
  for (size_t candidateIndex = 0u; candidateIndex < candidateLimit;
       ++candidateIndex) {
    const uint32_t candidate = static_cast<uint32_t>(candidateIndex);
    if (candidate == nodeIndex ||
        SceneHierarchyContainsNode(descendants, candidate)) {
      continue;
    }
    if (sceneGraph.getNode(candidate) == nullptr) {
      continue;
    }
    candidateCache.rows.push_back(SceneHierarchyParentCandidateRow{
        .parent = candidate,
        .label = SceneHierarchyNodeName(sceneGraph, candidate),
        .selected = currentParent == candidate});
  }
}

struct SceneHierarchyPanelActions {
  uint32_t rootSceneNode{container::scene::SceneGraph::kInvalidNode};
  uint32_t selectedMeshNode{container::scene::SceneGraph::kInvalidNode};
  const std::function<void(uint32_t)> *selectSceneNode{nullptr};
  const std::function<void(uint32_t)> *focusSceneNode{nullptr};
  const std::function<void(uint32_t, bool)> *setSceneNodeVisible{nullptr};
  const std::function<void(uint32_t, std::optional<uint32_t>)>
      *reparentSceneNode{nullptr};
};

struct SceneHierarchyParentChange {
  bool requested{false};
  uint32_t nodeIndex{container::scene::SceneGraph::kInvalidNode};
  std::optional<uint32_t> parent{};
};

void DrawSceneHierarchyNode(const container::scene::SceneGraph &sceneGraph,
                            uint32_t nodeIndex,
                            const SceneHierarchyPanelActions &actions,
                            SceneHierarchyParentChange &parentChange,
                            SceneHierarchyParentCandidateCacheState
                                &candidateCache);

SceneHierarchyParentChange DrawSceneHierarchyParentCombo(
    const container::scene::SceneGraph &sceneGraph, uint32_t nodeIndex,
    const container::scene::SceneNode &node,
    const SceneHierarchyPanelActions &actions,
    SceneHierarchyParentCandidateCacheState &candidateCache) {
  SceneHierarchyParentChange change{};
  const bool canReparent = nodeIndex != actions.rootSceneNode;
  if (!canReparent) {
    ImGui::BeginDisabled();
  }

  const uint32_t currentParent = node.parent;
  const std::string parentPreview =
      currentParent == container::scene::SceneGraph::kInvalidNode
          ? "None"
          : SceneHierarchyNodeName(sceneGraph, currentParent);
  const std::string parentComboId =
      "##SceneHierarchyParent" + std::to_string(nodeIndex);
  if (ImGui::BeginCombo(parentComboId.c_str(), parentPreview.c_str())) {
    RefreshSceneHierarchyParentCandidateCache(sceneGraph, nodeIndex,
                                              currentParent, candidateCache);
    ImGuiListClipper clipper;
    clipper.Begin(static_cast<int>(std::min<size_t>(
        candidateCache.rows.size(), static_cast<size_t>(
                                       std::numeric_limits<int>::max()))));
    while (clipper.Step() && !change.requested) {
      for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
        const auto &candidate = candidateCache.rows[static_cast<size_t>(row)];
        if (ImGui::Selectable(candidate.label.c_str(), candidate.selected)) {
          if (!candidate.selected) {
            change.requested = true;
            change.nodeIndex = nodeIndex;
            change.parent = candidate.parent;
          }
          ImGui::CloseCurrentPopup();
          break;
        }
        if (candidate.selected) {
          ImGui::SetItemDefaultFocus();
        }
      }
    }
    ImGui::EndCombo();
  }

  if (!canReparent) {
    ImGui::EndDisabled();
  }
  return change;
}

void DrawSceneHierarchyNode(const container::scene::SceneGraph &sceneGraph,
                            uint32_t nodeIndex,
                            const SceneHierarchyPanelActions &actions,
                            SceneHierarchyParentChange &parentChange,
                            SceneHierarchyParentCandidateCacheState
                                &candidateCache) {
  const auto *node = sceneGraph.getNode(nodeIndex);
  if (node == nullptr) {
    return;
  }

  ImGui::TableNextRow();
  ImGui::TableNextColumn();
  ImGuiTreeNodeFlags flags =
      ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanFullWidth;
  if (node->children.empty()) {
    flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
  }
  if (nodeIndex == actions.selectedMeshNode) {
    flags |= ImGuiTreeNodeFlags_Selected;
  }

  const std::string label = SceneHierarchyNodeName(sceneGraph, nodeIndex) +
                            "##SceneHierarchyNode" +
                            std::to_string(nodeIndex);
  const bool open = ImGui::TreeNodeEx(label.c_str(), flags);
  if (ImGui::IsItemClicked(ImGuiMouseButton_Left) &&
      !ImGui::IsItemToggledOpen() && actions.selectSceneNode != nullptr) {
    (*actions.selectSceneNode)(nodeIndex);
  }
  if (ImGui::IsItemHovered()) {
    const std::string tooltip = "Select node " + std::to_string(nodeIndex);
    ShowItemTooltip(tooltip);
  }

  ImGui::TableNextColumn();
  bool visible = node->visible;
  const std::string visibleId =
      "##SceneHierarchyVisible" + std::to_string(nodeIndex);
  if (ImGui::Checkbox(visibleId.c_str(), &visible) &&
      actions.setSceneNodeVisible != nullptr) {
    (*actions.setSceneNodeVisible)(nodeIndex, visible);
  }
  if (!sceneGraph.isNodeEffectivelyVisible(nodeIndex) && node->visible) {
    ImGui::SameLine();
    ImGui::TextDisabled("parent");
  }

  ImGui::TableNextColumn();
  const std::string focusId =
      "Focus##SceneHierarchyFocus" + std::to_string(nodeIndex);
  if (ImGui::SmallButton(focusId.c_str()) &&
      actions.focusSceneNode != nullptr) {
    (*actions.focusSceneNode)(nodeIndex);
  }

  ImGui::TableNextColumn();
  const std::vector<uint32_t> children = node->children;
  SceneHierarchyParentChange nodeParentChange =
      DrawSceneHierarchyParentCombo(sceneGraph, nodeIndex, *node, actions,
                                    candidateCache);
  if (!parentChange.requested && nodeParentChange.requested) {
    parentChange = nodeParentChange;
  }

  if (open && !children.empty()) {
    for (const uint32_t child : children) {
      DrawSceneHierarchyNode(sceneGraph, child, actions, parentChange,
                             candidateCache);
    }
    ImGui::TreePop();
  }
}

void DrawSceneHierarchyPanel(const container::scene::SceneGraph &sceneGraph,
                             const SceneHierarchyPanelActions &actions,
                             SceneHierarchyParentCandidateCacheState
                                 &candidateCache) {
  if (sceneGraph.nodeCount() == 0 || !ImGui::TreeNode("Scene Hierarchy")) {
    return;
  }

  if (ImGui::BeginTable("SceneHierarchyTable", 4,
                        ImGuiTableFlags_BordersInnerV |
                            ImGuiTableFlags_RowBg |
                            ImGuiTableFlags_Resizable |
                            ImGuiTableFlags_SizingStretchProp)) {
    SceneHierarchyParentChange parentChange{};
    ImGui::TableSetupColumn("Node", ImGuiTableColumnFlags_WidthStretch, 0.48f);
    ImGui::TableSetupColumn("Visible", ImGuiTableColumnFlags_WidthFixed,
                            82.0f);
    ImGui::TableSetupColumn("Focus", ImGuiTableColumnFlags_WidthFixed, 72.0f);
    ImGui::TableSetupColumn("Parent", ImGuiTableColumnFlags_WidthStretch,
                            0.32f);
    ImGui::TableHeadersRow();
    for (const uint32_t root : sceneGraph.rootNodes()) {
      DrawSceneHierarchyNode(sceneGraph, root, actions, parentChange,
                             candidateCache);
    }
    ImGui::EndTable();
    if (parentChange.requested && actions.reparentSceneNode != nullptr) {
      (*actions.reparentSceneNode)(parentChange.nodeIndex,
                                   parentChange.parent);
    }
  }
  ImGui::TreePop();
}

bool IsGltfModelFile(const std::filesystem::path &path) {
  const std::string extension =
      ToLowerAscii(container::util::pathToUtf8(path.extension()));
  return extension == ".gltf" || extension == ".glb";
}

bool IsAuxiliaryModelFile(const std::filesystem::path &path) {
  const std::string extension =
      ToLowerAscii(container::util::pathToUtf8(path.extension()));
  return extension == ".usd" || extension == ".usda" || extension == ".usdc" ||
         extension == ".usdz" || extension == ".ifc" || extension == ".ifcx" ||
         extension == ".bim";
}

std::optional<std::filesystem::path>
ResolveRelativeAssetPath(std::string_view relativePath) {
  const auto relative =
      container::util::pathFromUtf8(std::string(relativePath));
  const std::array<std::filesystem::path, 2> candidates = {
      container::util::executableDirectory() / relative,
      std::filesystem::current_path() / relative,
  };

  for (const auto &candidate : candidates) {
    std::error_code error;
    if (std::filesystem::is_regular_file(candidate, error)) {
      return candidate;
    }
  }

  return std::nullopt;
}

std::optional<std::filesystem::path>
ResolveRelativeAssetDirectory(std::string_view relativePath) {
  const auto relative =
      container::util::pathFromUtf8(std::string(relativePath));
  const std::array<std::filesystem::path, 2> candidates = {
      container::util::executableDirectory() / relative,
      std::filesystem::current_path() / relative,
  };

  for (const auto &candidate : candidates) {
    std::error_code error;
    if (std::filesystem::is_directory(candidate, error)) {
      return candidate;
    }
  }

  return std::nullopt;
}

std::filesystem::path ComparableModelPath(const std::filesystem::path &input) {
  std::filesystem::path path = input;
  if (path.is_relative()) {
    std::error_code existsError;
    const auto exeRelative =
        container::util::executableDirectory() / std::filesystem::path(path);
    if (std::filesystem::exists(exeRelative, existsError)) {
      path = exeRelative;
    } else {
      std::error_code currentPathError;
      const auto workingDirectory =
          std::filesystem::current_path(currentPathError);
      if (!currentPathError) {
        path = workingDirectory / path;
      }
    }
  }

  std::error_code canonicalError;
  const auto canonical =
      std::filesystem::weakly_canonical(path, canonicalError);
  if (!canonicalError) {
    return canonical.lexically_normal();
  }

  std::error_code absoluteError;
  const auto absolute = std::filesystem::absolute(path, absoluteError);
  if (!absoluteError) {
    return absolute.lexically_normal();
  }

  return path.lexically_normal();
}

bool TextFileContains(const std::filesystem::path &path,
                      std::string_view needle) {
  std::ifstream file(path, std::ios::binary);
  if (!file.is_open()) {
    return false;
  }

  constexpr size_t kBufferSize = 64u * 1024u;
  std::string carry;
  carry.reserve(needle.size());
  std::array<char, kBufferSize> buffer{};
  while (file) {
    file.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const std::streamsize read = file.gcount();
    if (read <= 0) {
      break;
    }

    std::string chunk = carry;
    chunk.append(buffer.data(), static_cast<size_t>(read));
    if (chunk.find(needle) != std::string::npos) {
      return true;
    }

    const size_t carrySize = std::min(needle.size(), chunk.size());
    carry.assign(chunk.end() - static_cast<std::ptrdiff_t>(carrySize),
                 chunk.end());
  }

  return false;
}

bool heuristicRenderableIfc5Layer(const std::filesystem::path &path) {
  const std::filesystem::path dir = path.parent_path();
  const std::string stem = container::util::pathToUtf8(path.stem());
  const std::string lowerStem = ToLowerAscii(stem);
  if (const size_t add = lowerStem.find("-add-");
      add != std::string::npos && add > 0u) {
    const std::filesystem::path candidate =
        dir / container::util::pathFromUtf8(stem.substr(0, add) + ".ifcx");
    std::error_code existsError;
    return std::filesystem::exists(candidate, existsError);
  }

  if ((lowerStem.starts_with("add-") ||
       ToLowerAscii(container::util::pathToUtf8(dir.filename())) ==
           "advanced") &&
      dir.has_parent_path()) {
    std::error_code iterError;
    for (const std::filesystem::directory_entry &entry :
         std::filesystem::directory_iterator(dir.parent_path(), iterError)) {
      if (iterError) {
        break;
      }
      const std::string candidateExtension =
          ToLowerAscii(container::util::pathToUtf8(entry.path().extension()));
      if (!entry.is_regular_file() || candidateExtension != ".ifcx") {
        continue;
      }
      const std::string candidateStem =
          ToLowerAscii(container::util::pathToUtf8(entry.path().stem()));
      if (candidateStem.find("-add-") == std::string::npos &&
          !candidateStem.starts_with("add-")) {
        return true;
      }
    }
  }
  return false;
}

bool HasRenderableAuxiliaryHint(const std::filesystem::path &path) {
  const std::string extension =
      ToLowerAscii(container::util::pathToUtf8(path.extension()));
  if (extension == ".ifcx") {
    return TextFileContains(path, "usd::usdgeom::mesh") ||
           TextFileContains(path, "usd::usdgeom::basiscurves") ||
           TextFileContains(path, "points::array") ||
           TextFileContains(path, "points::base64") ||
           TextFileContains(path, "pcd::base64") ||
           heuristicRenderableIfc5Layer(path);
  }
  if (extension == ".ifc") {
    // The native importer also supports BReps, sweeps, curves and surfaces.
    // Two token hints no longer describe its supported representation families.
    return true;
  }
  return true;
}

std::string AuxiliarySampleLabel(std::string_view prefix,
                                 const std::filesystem::path &root,
                                 const std::filesystem::path &path) {
  std::error_code error;
  std::filesystem::path relative = std::filesystem::relative(path, root, error);
  if (error) {
    relative = path.filename();
  }
  relative.replace_extension();

  std::string relativeLabel = container::util::pathToUtf8(relative);
  for (size_t pos = 0;
       (pos = relativeLabel.find_first_of("/\\", pos)) != std::string::npos;) {
    relativeLabel.replace(pos, 1, " / ");
    pos += 3;
  }

  std::string label(prefix);
  label += " / ";
  label += relativeLabel;
  if (ToLowerAscii(container::util::pathToUtf8(path.extension())) == ".ifc")
    label += " (STEP)";
  return label;
}

std::vector<DiscoveredSampleAsset> DiscoverAuxiliarySampleAssets() {
  std::vector<DiscoveredSampleAsset> assets;
  for (const KnownSampleDirectory &directory :
       kKnownAuxiliarySampleDirectories) {
    const auto root = ResolveRelativeAssetDirectory(directory.relativePath);
    if (!root) {
      continue;
    }

    std::error_code error;
    for (std::filesystem::recursive_directory_iterator it(
             *root, std::filesystem::directory_options::skip_permission_denied,
             error),
         end;
         !error && it != end; it.increment(error)) {
      std::error_code fileError;
      if (!it->is_regular_file(fileError) ||
          !IsAuxiliaryModelFile(it->path())) {
        continue;
      }

      if (!HasRenderableAuxiliaryHint(it->path())) {
        continue;
      }

      assets.push_back({
          AuxiliarySampleLabel(directory.labelPrefix, *root, it->path()),
          it->path(),
      });
    }
  }

  std::ranges::sort(assets, {}, &DiscoveredSampleAsset::label);
  return assets;
}

std::string ModelPathKey(const std::filesystem::path &path) {
  std::string key = container::util::pathToUtf8(ComparableModelPath(path));
#ifdef _WIN32
  key = ToLowerAscii(std::move(key));
#endif
  return key;
}

std::optional<std::filesystem::path> ResolveSampleModelsRoot() {
  const std::array<std::filesystem::path, 2> candidates = {
      container::util::executableDirectory() /
          std::filesystem::path(kSampleModelsRelativeRoot),
      std::filesystem::current_path() /
          std::filesystem::path(kSampleModelsRelativeRoot),
  };

  for (const auto &candidate : candidates) {
    std::error_code error;
    if (std::filesystem::is_directory(candidate, error)) {
      return candidate;
    }
  }

  return std::nullopt;
}

std::optional<std::filesystem::path>
FindFirstModelFileInDirectory(const std::filesystem::path &directory) {
  std::error_code error;
  if (!std::filesystem::is_directory(directory, error)) {
    return std::nullopt;
  }

  std::vector<std::filesystem::path> candidates;
  for (std::filesystem::directory_iterator it(directory, error), end;
       !error && it != end; it.increment(error)) {
    std::error_code fileError;
    if (it->is_regular_file(fileError) && IsGltfModelFile(it->path())) {
      candidates.push_back(it->path());
    }
  }

  if (candidates.empty()) {
    return std::nullopt;
  }

  std::ranges::sort(candidates);
  const auto gltf = std::ranges::find_if(candidates, [](const auto &path) {
    return ToLowerAscii(container::util::pathToUtf8(path.extension())) ==
           ".gltf";
  });
  return gltf != candidates.end() ? *gltf : candidates.front();
}

std::optional<std::filesystem::path>
FindFirstModelFileRecursive(const std::filesystem::path &directory) {
  std::error_code error;
  std::vector<std::filesystem::path> candidates;
  for (std::filesystem::recursive_directory_iterator it(
           directory,
           std::filesystem::directory_options::skip_permission_denied, error),
       end;
       !error && it != end; it.increment(error)) {
    std::error_code fileError;
    if (it->is_regular_file(fileError) && IsGltfModelFile(it->path())) {
      candidates.push_back(it->path());
    }
  }

  if (candidates.empty()) {
    return std::nullopt;
  }

  std::ranges::sort(candidates);
  return candidates.front();
}

std::optional<std::filesystem::path>
PreferredSampleModelPath(const std::filesystem::path &modelDirectory,
                         std::string &selectedVariant) {
  static constexpr std::array<std::string_view, 6> kPreferredVariantFolders = {{
      "glTF",
      "glTF-Binary",
      "glTF-Embedded",
      "glTF-MaterialsCommon",
      "glTF-pbrSpecularGlossiness",
      "glTF-KTX-BasisU",
  }};

  for (std::string_view variant : kPreferredVariantFolders) {
    const auto variantDirectory =
        modelDirectory / std::filesystem::path(variant);
    if (auto path = FindFirstModelFileInDirectory(variantDirectory)) {
      selectedVariant = std::string(variant);
      return path;
    }
  }

  if (auto path = FindFirstModelFileRecursive(modelDirectory)) {
    selectedVariant =
        container::util::pathToUtf8(path->parent_path().filename());
    return path;
  }

  return std::nullopt;
}

std::string ImportScaleLabel(float scale) {
  for (size_t i = 0; i < kImportScaleValues.size(); ++i) {
    if (scale == kImportScaleValues[i]) {
      return kImportScaleLabels[i];
    }
  }
  return std::to_string(scale) + "x";
}

void CheckVkResult(VkResult result) {
  if (result != VK_SUCCESS) {
    throw std::runtime_error("ImGui Vulkan backend error");
  }
}

constexpr size_t TelemetryPhaseIndex(GuiRendererTelemetryPhase phase) {
  return static_cast<size_t>(phase);
}

float TelemetryPhaseMs(const GuiRendererTelemetrySnapshot &snapshot,
                       GuiRendererTelemetryPhase phase) {
  if (phase == GuiRendererTelemetryPhase::Count) {
    return 0.0f;
  }
  return snapshot.timing.cpuMs[TelemetryPhaseIndex(phase)];
}

constexpr GuiRendererTelemetryPhase
GuiTelemetryPhase(container::renderer::RendererTelemetryPhase phase) {
  switch (phase) {
  case container::renderer::RendererTelemetryPhase::Frame:
    return GuiRendererTelemetryPhase::Frame;
  case container::renderer::RendererTelemetryPhase::WaitForFrame:
    return GuiRendererTelemetryPhase::WaitForFrame;
  case container::renderer::RendererTelemetryPhase::Readbacks:
    return GuiRendererTelemetryPhase::Readbacks;
  case container::renderer::RendererTelemetryPhase::AcquireImage:
    return GuiRendererTelemetryPhase::AcquireImage;
  case container::renderer::RendererTelemetryPhase::ImageFenceWait:
    return GuiRendererTelemetryPhase::ImageFenceWait;
  case container::renderer::RendererTelemetryPhase::ResourceGrowth:
    return GuiRendererTelemetryPhase::ResourceGrowth;
  case container::renderer::RendererTelemetryPhase::Gui:
    return GuiRendererTelemetryPhase::Gui;
  case container::renderer::RendererTelemetryPhase::SceneUpdate:
    return GuiRendererTelemetryPhase::SceneUpdate;
  case container::renderer::RendererTelemetryPhase::DescriptorUpdate:
    return GuiRendererTelemetryPhase::DescriptorUpdate;
  case container::renderer::RendererTelemetryPhase::CommandRecord:
    return GuiRendererTelemetryPhase::CommandRecord;
  case container::renderer::RendererTelemetryPhase::QueueSubmit:
    return GuiRendererTelemetryPhase::QueueSubmit;
  case container::renderer::RendererTelemetryPhase::Screenshot:
    return GuiRendererTelemetryPhase::Screenshot;
  case container::renderer::RendererTelemetryPhase::Present:
    return GuiRendererTelemetryPhase::Present;
  case container::renderer::RendererTelemetryPhase::Count:
    return GuiRendererTelemetryPhase::Count;
  }
  return GuiRendererTelemetryPhase::Count;
}

GuiRendererTelemetryView GuiRendererTelemetry(
    const container::renderer::RendererTelemetryView &telemetry) {
  GuiRendererTelemetryView uiTelemetry{};
  uiTelemetry.summary = {
      .frameCount = telemetry.summary.frameCount,
      .averageCpuFrameMs = telemetry.summary.averageCpuFrameMs,
      .p95CpuFrameMs = telemetry.summary.p95CpuFrameMs,
      .maxCpuFrameMs = telemetry.summary.maxCpuFrameMs,
      .averageGpuKnownMs = telemetry.summary.averageGpuKnownMs,
      .maxGpuKnownMs = telemetry.summary.maxGpuKnownMs};
  uiTelemetry.history.reserve(telemetry.history.size());
  for (const auto &sample : telemetry.history) {
    uiTelemetry.history.push_back({.frameIndex = sample.frameIndex,
                                   .cpuFrameMs = sample.cpuFrameMs,
                                   .gpuKnownMs = sample.gpuKnownMs,
                                   .waitForFrameMs = sample.waitForFrameMs,
                                   .presentMs = sample.presentMs});
  }

  const auto &source = telemetry.latest;
  auto &latest = uiTelemetry.latest;
  latest.valid = source.valid;
  latest.frameIndex = source.frameIndex;
  latest.imageIndex = source.imageIndex;
  for (size_t i = 0; i < container::renderer::kRendererTelemetryPhaseCount;
       ++i) {
    const auto phase =
        static_cast<container::renderer::RendererTelemetryPhase>(i);
    latest.timing.cpuMs[TelemetryPhaseIndex(GuiTelemetryPhase(phase))] =
        source.timing.cpuMs[i];
  }
  latest.timing.gpuKnownMs = source.timing.gpuKnownMs;
  latest.timing.gpuSource =
      std::string(container::renderer::rendererGpuTimingSourceName(
          source.timing.gpuSource));
  latest.culling = {.inputCount = source.culling.inputCount,
                    .frustumPassedCount = source.culling.frustumPassedCount,
                    .occlusionPassedCount =
                        source.culling.occlusionPassedCount};
  latest.lightCulling = {
      .submittedLights = source.lightCulling.submittedLights,
      .activeClusters = source.lightCulling.activeClusters,
      .totalClusters = source.lightCulling.totalClusters,
      .maxLightsPerCluster = source.lightCulling.maxLightsPerCluster,
      .droppedLightReferences = source.lightCulling.droppedLightReferences,
      .clusterCullMs = source.lightCulling.clusterCullMs,
      .clusteredLightingMs = source.lightCulling.clusteredLightingMs};
  latest.workload = {.objectCount = source.workload.objectCount,
                     .opaqueDrawCount = source.workload.opaqueDrawCount,
                     .transparentDrawCount =
                         source.workload.transparentDrawCount,
                     .totalDrawCount = source.workload.totalDrawCount,
                     .submittedLights = source.workload.submittedLights};
  latest.resources = {
      .swapchainWidth = source.resources.swapchainWidth,
      .swapchainHeight = source.resources.swapchainHeight,
      .swapchainImageCount = source.resources.swapchainImageCount,
      .cameraBufferCount = source.resources.cameraBufferCount,
      .objectBufferCapacity = source.resources.objectBufferCapacity,
      .oitNodeCapacity = source.resources.oitNodeCapacity};
  latest.sync = {.frameSlot = source.sync.frameSlot,
                 .maxFramesInFlight = source.sync.maxFramesInFlight,
                 .serializedConcurrency = source.sync.serializedConcurrency,
                 .concurrencyReason = source.sync.concurrencyReason,
                 .swapchainRecreateCount = source.sync.swapchainRecreateCount,
                 .deviceWaitIdleCount = source.sync.deviceWaitIdleCount};
  latest.graph = {.totalPasses = source.graph.totalPasses,
                  .enabledPasses = source.graph.enabledPasses,
                  .activePasses = source.graph.activePasses,
                  .skippedPasses = source.graph.skippedPasses,
                  .cpuTimedPasses = source.graph.cpuTimedPasses,
                  .gpuTimedPasses = source.graph.gpuTimedPasses};
  latest.gpuProfiler = {
      .source = std::string(container::renderer::rendererGpuTimingSourceName(
          source.gpuProfiler.source)),
      .available = source.gpuProfiler.available,
      .resultLatencyFrames = source.gpuProfiler.resultLatencyFrames,
      .status = source.gpuProfiler.status};
  latest.passes.reserve(source.passes.size());
  for (const auto &pass : source.passes) {
    latest.passes.push_back({.name = pass.name,
                             .enabled = pass.enabled,
                             .active = pass.active,
                             .cpuTimed = pass.cpuTimed,
                             .gpuTimed = pass.gpuTimed,
                             .cpuRecordMs = pass.cpuRecordMs,
                             .gpuKnownMs = pass.gpuKnownMs,
                             .status = pass.status,
                             .blocker = pass.blocker});
  }
  return uiTelemetry;
}

float PercentOf(float value, float total) {
  return total > 0.0f ? (value / total) * 100.0f : 0.0f;
}

void ImGuiTextStringView(const char *label, std::string_view value) {
  ImGui::Text("%s: %.*s", label, static_cast<int>(value.size()), value.data());
}

bool DrawTransformControls(const char *label, TransformControls &transform,
                           float dragSpeed = 0.05f) {
  bool changed = false;
  if (ImGui::TreeNode(label)) {
    changed |= ImGui::DragFloat3("Position", &transform.position.x, dragSpeed);
    changed |=
        ImGui::DragFloat3("Rotation", &transform.rotationDegrees.x, 0.5f);
    changed |= ImGui::DragFloat3("Scale", &transform.scale.x, dragSpeed, 0.001f,
                                 1000.0f);
    ImGui::TreePop();
  }
  return changed;
}

void DrawLightInspectorSection(const char *label) {
  ImGui::Spacing();
  ImGui::Separator();
  ImGui::TextUnformatted(label);
}

bool DrawEditableLightInspector(EditableLightEntity &light) {
  bool changed = false;
  ImGui::TextUnformatted("Selected Light Properties");
  ImGui::Text("Source: %s",
              container::renderer::editableLightSourceLabel(light.source));
  ImGui::Text("Type: %s",
              container::renderer::editableLightTypeLabel(light.type));

  if (light.source == EditableLightSource::Generated) {
    ImGui::TextDisabled(
        "Live generated light; generator changes may overwrite edits");
  }

  DrawLightInspectorSection("Light Transform");
  if (light.type != EditableLightType::Directional) {
    changed |= ImGui::DragFloat3("Position", &light.position.x, 0.05f);
    changed |=
        ImGui::DragFloat("Range", &light.range, 0.05f, 0.0f, 100000.0f, "%.3f");
  }

  if (light.type == EditableLightType::Directional) {
    changed |=
        ImGui::DragFloat3("Directional direction", &light.direction.x, 0.01f);
  } else if (light.type == EditableLightType::Spot ||
             light.type == EditableLightType::Area) {
    changed |= ImGui::DragFloat3("Direction", &light.direction.x, 0.01f);
  }

  DrawLightInspectorSection("Light Radiance");
  changed |= ImGui::ColorEdit3("Color", &light.color.x);
  changed |= ImGui::DragFloat("Intensity", &light.intensity, 0.05f, 0.0f,
                              100000.0f, "%.3f");

  if (light.type == EditableLightType::Spot) {
    DrawLightInspectorSection("Spot Beam");
    changed |= ImGui::SliderFloat("Inner cone", &light.innerConeDegrees, 0.0f,
                                  179.0f, "%.1f deg");
    changed |= ImGui::SliderFloat("Outer cone", &light.outerConeDegrees, 0.0f,
                                  179.0f, "%.1f deg");
    if (light.outerConeDegrees < light.innerConeDegrees) {
      light.outerConeDegrees = light.innerConeDegrees;
      changed = true;
    }
  }

  if (light.type == EditableLightType::Area) {
    DrawLightInspectorSection("Area Emitter");
    static constexpr const char *kAreaShapeLabels[] = {"Rectangle", "Disk"};
    int shape =
        light.areaShape >= (container::gpu::kAreaLightTypeDisk - 0.5f) ? 1 : 0;
    if (ImGui::Combo("Area shape", &shape, kAreaShapeLabels,
                     IM_ARRAYSIZE(kAreaShapeLabels))) {
      light.areaShape = shape == 1 ? container::gpu::kAreaLightTypeDisk
                                   : container::gpu::kAreaLightTypeRectangle;
      changed = true;
    }
    float areaWidth = light.areaHalfSize.x * 2.0f;
    float areaHeight = light.areaHalfSize.y * 2.0f;
    if (ImGui::DragFloat("Area Width", &areaWidth, 0.05f, 0.002f, 200000.0f,
                         "%.3f")) {
      light.areaHalfSize.x = std::max(areaWidth * 0.5f, 0.001f);
      changed = true;
    }
    if (ImGui::DragFloat("Area Height", &areaHeight, 0.05f, 0.002f,
                         200000.0f, "%.3f")) {
      light.areaHalfSize.y = std::max(areaHeight * 0.5f, 0.001f);
      changed = true;
    }
    changed |= ImGui::DragFloat3("Area Tangent", &light.tangent.x, 0.01f);
    changed |= ImGui::DragFloat3("Area Bitangent", &light.bitangent.x, 0.01f);
  }

  DrawLightInspectorSection("Shadow Eligibility");
  if (light.type == EditableLightType::Directional) {
    ImGui::TextUnformatted("Uses directional cascaded shadows when enabled.");
  } else if (light.type == EditableLightType::Point ||
             light.type == EditableLightType::Spot) {
    const bool eligible = light.intensity > 0.0f && light.range > 0.0f;
    ImGui::Text("Eligible for local shadow maps: %s",
                eligible ? "yes" : "no");
    ImGui::TextWrapped(
        "Local shadow budget assigns the first eligible point/spot lights.");
  } else {
    ImGui::TextUnformatted(
        "Area light visibility uses the renderer local-shadow assignment.");
  }

  return changed;
}

const char *ViewportGestureLabel(ViewportGesture gesture) {
  switch (gesture) {
  case ViewportGesture::None:
    return "Idle";
  case ViewportGesture::FlyLook:
    return "Fly";
  case ViewportGesture::Orbit:
    return "Orbit";
  case ViewportGesture::Pan:
    return "Pan";
  case ViewportGesture::TransformDrag:
    return "Transform";
  }
  return "Idle";
}

bool ToolButton(const char *label, ViewportTool tool, ViewportTool activeTool,
                const char *tooltip = nullptr) {
  if (tool == activeTool) {
    ImGui::PushStyleColor(ImGuiCol_Button,
                          ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
  }
  const bool clicked = ImGui::Button(label);
  if (tool == activeTool) {
    ImGui::PopStyleColor();
  }
  if (tooltip != nullptr) {
    ShowItemTooltip(tooltip);
  }
  return clicked;
}

const char *TransformAxisLabel(TransformAxis axis) {
  switch (axis) {
  case TransformAxis::Free:
    return "Free";
  case TransformAxis::X:
    return "X";
  case TransformAxis::Y:
    return "Y";
  case TransformAxis::Z:
    return "Z";
  }
  return "Free";
}

const char *CameraViewPresetLabel(CameraViewPreset preset) {
  switch (preset) {
  case CameraViewPreset::Front:
    return "Front";
  case CameraViewPreset::Back:
    return "Back";
  case CameraViewPreset::Right:
    return "Right";
  case CameraViewPreset::Left:
    return "Left";
  case CameraViewPreset::Top:
    return "Top";
  case CameraViewPreset::Bottom:
    return "Bottom";
  }
  return "Front";
}

const char *BimElevationTechnicalStyleLabel(BimElevationTechnicalStyle style) {
  switch (style) {
  case BimElevationTechnicalStyle::Shaded:
    return "Shaded";
  case BimElevationTechnicalStyle::ShadedWithLines:
    return "Technical";
  case BimElevationTechnicalStyle::HiddenLine:
    return "Hidden line";
  }
  return "Technical";
}

void DrawRenderPassToggleEntry(RenderPassToggle &pass) {
  if (pass.locked) {
    ImGui::BeginDisabled();
    ImGui::Checkbox(pass.name.c_str(), &pass.enabled);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("core");
    return;
  }

  ImGui::Checkbox(pass.name.c_str(), &pass.enabled);
  if (pass.autoDisabled) {
    ImGui::SameLine();
    ImGui::TextDisabled("%s", pass.dependencyNote.empty()
                                  ? "disabled by dependency"
                                  : pass.dependencyNote.c_str());
  }
}

std::string_view RenderPassSectionName(std::string_view passName) {
  if (passName == "FrustumCull" || passName == "DepthPrepass" ||
      passName == "HiZGenerate" || passName == "OcclusionCull" ||
      passName == "CullStatsReadback" || passName == "GBuffer" ||
      passName == "DepthToReadOnly") {
    return "Culling";
  }

  if (passName == "ShadowCascade0" || passName == "ShadowCascade1" ||
      passName == "ShadowCascade2" || passName == "ShadowCascade3") {
    return "Shadows";
  }

  if (passName == "OitClear" || passName == "TileCull" || passName == "GTAO" ||
      passName == "Lighting" || passName == "OitResolve") {
    return "Lighting";
  }

  return "Post-process";
}

std::string ViewpointSnapshotDisplayName(const ViewpointSnapshotState &snapshot,
                                         size_t fallbackIndex) {
  if (!snapshot.label.empty()) {
    return snapshot.label;
  }
  return "Viewpoint " + std::to_string(fallbackIndex + 1u);
}

std::string CompactIdentifier(std::string_view value, size_t maxLength = 72u) {
  if (value.size() <= maxLength) {
    return std::string(value);
  }
  if (maxLength <= 3u) {
    return std::string(value.substr(0u, maxLength));
  }
  const size_t prefixLength = (maxLength - 3u) / 2u;
  const size_t suffixLength = maxLength - 3u - prefixLength;
  return std::string(value.substr(0u, prefixLength)) + "..." +
         std::string(value.substr(value.size() - suffixLength));
}

std::string
ViewpointSnapshotSelectionLabel(const ViewpointSnapshotState &snapshot) {
  if (!snapshot.selectedBimGuid.empty()) {
    return snapshot.selectedBimType.empty()
               ? snapshot.selectedBimGuid
               : snapshot.selectedBimType + " [" + snapshot.selectedBimGuid +
                     "]";
  }
  if (!snapshot.selectedBimSourceId.empty()) {
    const std::string sourceId =
        CompactIdentifier(snapshot.selectedBimSourceId);
    return snapshot.selectedBimType.empty()
               ? sourceId
               : snapshot.selectedBimType + " [" + sourceId + "]";
  }
  if (!snapshot.selectedBimType.empty()) {
    return snapshot.selectedBimType;
  }
  if (snapshot.selectedBimObjectIndex != std::numeric_limits<uint32_t>::max()) {
    return "BIM object " + std::to_string(snapshot.selectedBimObjectIndex);
  }
  if (snapshot.selectedMeshNode != std::numeric_limits<uint32_t>::max()) {
    return "Scene node " + std::to_string(snapshot.selectedMeshNode);
  }
  return "None";
}

std::string ViewpointModelPathKey(std::string_view value) {
  if (value.empty()) {
    return {};
  }

  const std::filesystem::path path =
      container::util::pathFromUtf8(std::string(value));
  std::string key = container::util::pathToUtf8(ComparableModelPath(path));
  std::ranges::replace(key, '\\', '/');
#ifdef _WIN32
  key = ToLowerAscii(std::move(key));
#endif
  return key;
}

bool ViewpointSnapshotModelMismatch(const ViewpointSnapshotState &snapshot,
                                    const BimInspectionState &inspection) {
  if (!inspection.hasScene || inspection.modelPath.empty() ||
      snapshot.bimModelPath.empty()) {
    return false;
  }
  return ViewpointModelPathKey(snapshot.bimModelPath) !=
         ViewpointModelPathKey(inspection.modelPath);
}

float BimMeasurementExtent(float extent) {
  return std::isfinite(extent) ? std::max(extent, 0.0f) : 0.0f;
}

glm::vec3 BimMeasurementDimensions(const glm::vec3 &size) {
  return {BimMeasurementExtent(size.x), BimMeasurementExtent(size.y),
          BimMeasurementExtent(size.z)};
}

float BimMeasurementFootprintArea(const glm::vec3 &dimensions) {
  return dimensions.x * dimensions.z;
}

float BimMeasurementVolume(const glm::vec3 &dimensions) {
  return dimensions.x * dimensions.y * dimensions.z;
}

constexpr std::array<const char *, 6> kBimMeasurementSnapModeLabels{{
    "Off",
    "Vertex",
    "Edge",
    "Face",
    "Bounds",
    "Floor",
}};

const char *BimMeasurementSnapModeLabel(BimMeasurementSnapMode mode) {
  const auto index = static_cast<size_t>(mode);
  if (index >= kBimMeasurementSnapModeLabels.size()) {
    return kBimMeasurementSnapModeLabels.front();
  }
  return kBimMeasurementSnapModeLabels[index];
}

std::string BimMeasurementSelectionLabel(const BimInspectionState &inspection) {
  if (!inspection.displayName.empty()) {
    return inspection.displayName;
  }

  std::string label = inspection.type.empty() ? "BIM object" : inspection.type;
  label += " ";
  label += std::to_string(inspection.selectedObjectIndex);
  if (!inspection.guid.empty()) {
    label += " [";
    label += CompactIdentifier(inspection.guid, 36u);
    label += "]";
  } else if (!inspection.sourceId.empty()) {
    label += " [";
    label += CompactIdentifier(inspection.sourceId, 36u);
    label += "]";
  }
  return label;
}

std::optional<BimMeasurementCapturedPoint>
captureBimMeasurementPointFromSelection(
    const BimInspectionState &inspection,
    const BimMeasurementSnapUiState &snapState) {
  if (!inspection.hasSelectionBounds) {
    return std::nullopt;
  }

  BimMeasurementCapturedPoint captured{
      .center = inspection.selectionBoundsCenter,
      .objectIndex = inspection.selectedObjectIndex,
      .label = BimMeasurementSelectionLabel(inspection),
      .modelPath = inspection.modelPath,
      .snapKind = container::renderer::BimSnapKind::BoundsCenter,
  };
  if (snapState.mode == BimMeasurementSnapMode::Off) {
    return captured;
  }

  using container::renderer::BimSnapKind;
  static constexpr std::array kVertexKinds{BimSnapKind::BoundsCorner};
  static constexpr std::array kEdgeKinds{BimSnapKind::EdgeMidpoint};
  static constexpr std::array kFaceKinds{BimSnapKind::FaceCenter};
  static constexpr std::array kBoundsKinds{BimSnapKind::BoundsCorner,
                                           BimSnapKind::BoundsCenter};
  static constexpr std::array kFloorKinds{BimSnapKind::FloorElevation};

  std::span<const BimSnapKind> allowedKinds{};
  switch (snapState.mode) {
  case BimMeasurementSnapMode::Vertex:
    allowedKinds = std::span<const BimSnapKind>{kVertexKinds};
    break;
  case BimMeasurementSnapMode::Edge:
    allowedKinds = std::span<const BimSnapKind>{kEdgeKinds};
    break;
  case BimMeasurementSnapMode::Face:
    allowedKinds = std::span<const BimSnapKind>{kFaceKinds};
    break;
  case BimMeasurementSnapMode::Bounds:
    allowedKinds = std::span<const BimSnapKind>{kBoundsKinds};
    break;
  case BimMeasurementSnapMode::Floor:
    allowedKinds = std::span<const BimSnapKind>{kFloorKinds};
    break;
  case BimMeasurementSnapMode::Off:
    return captured;
  }

  const container::renderer::BimBoundsSnapInput snapInput{
      .objectIndex = inspection.selectedObjectIndex,
      .min = inspection.selectionBoundsMin,
      .max = inspection.selectionBoundsMax,
      .floorElevation = inspection.selectionFloorElevation,
      .screenDistancePixels = 0.0f,
      .label = captured.label,
      .includeFloorElevation = true,
  };
  const std::vector<container::renderer::BimSnapCandidate> candidates =
      container::renderer::BuildBimBoundsSnapCandidates(snapInput);
  const std::optional<container::renderer::BimSnapCandidate> best =
      container::renderer::BestBimSnapCandidate(
          candidates, snapState.maxScreenDistancePixels, allowedKinds);
  if (!best.has_value()) {
    return captured;
  }

  captured.center = best->worldPosition;
  captured.objectIndex = best->objectIndex;
  captured.label = best->label.empty() ? captured.label : best->label;
  captured.snapKind = best->kind;
  return captured;
}

const char *BimModelCompareChangeKindLabel(
    container::renderer::BimModelCompareChangeKind kind) {
  using container::renderer::BimModelCompareChangeKind;
  switch (kind) {
  case BimModelCompareChangeKind::Added:
    return "Added";
  case BimModelCompareChangeKind::Removed:
    return "Removed";
  case BimModelCompareChangeKind::ChangedType:
    return "Type";
  case BimModelCompareChangeKind::ChangedStorey:
    return "Storey";
  case BimModelCompareChangeKind::ChangedMaterial:
    return "Material";
  case BimModelCompareChangeKind::ChangedBounds:
    return "Bounds";
  }
  return "Changed";
}

void DrawBimCoordinateReadout(
    const container::renderer::BimCoordinateReadout &readout) {
  ImGui::Text("Renderer: %.3f, %.3f, %.3f", readout.rendererWorld.x,
              readout.rendererWorld.y, readout.rendererWorld.z);
  ImGui::Text("Project: %.3f, %.3f, %.3f", readout.projectCoordinates.x,
              readout.projectCoordinates.y, readout.projectCoordinates.z);
  if (readout.hasSurveyCoordinates) {
    ImGui::Text("Survey: %.3f, %.3f, %.3f", readout.surveyCoordinates.x,
                readout.surveyCoordinates.y, readout.surveyCoordinates.z);
  }
  if (!readout.crsLabel.empty()) {
    ImGui::TextWrapped("CRS readout: %s", readout.crsLabel.c_str());
  }
}

void DrawBimScheduleRows(std::string_view tableId,
                         std::span<const container::renderer::BimScheduleRow>
                             rows) {
  if (rows.empty()) {
    ImGui::TextDisabled("No rows");
    return;
  }
  if (ImGui::BeginTable(std::string(tableId).c_str(), 5,
                        ImGuiTableFlags_BordersInnerV |
                            ImGuiTableFlags_RowBg |
                            ImGuiTableFlags_SizingStretchProp)) {
    ImGui::TableSetupColumn("Key");
    ImGui::TableSetupColumn("Storey");
    ImGui::TableSetupColumn("Count");
    ImGui::TableSetupColumn("Area");
    ImGui::TableSetupColumn("Volume");
    ImGui::TableHeadersRow();
    const size_t visibleRows = std::min<size_t>(rows.size(), 24u);
    for (size_t i = 0; i < visibleRows; ++i) {
      const auto &row = rows[i];
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::TextWrapped("%s", row.key.c_str());
      ImGui::TableNextColumn();
      ImGui::TextWrapped("%s", row.storey.empty() ? "-" : row.storey.c_str());
      ImGui::TableNextColumn();
      ImGui::Text("%u", row.count);
      ImGui::TableNextColumn();
      ImGui::Text("%.2f", row.estimatedArea);
      ImGui::TableNextColumn();
      ImGui::Text("%.2f", row.estimatedVolume);
    }
    ImGui::EndTable();
    if (visibleRows < rows.size()) {
      ImGui::TextDisabled("%zu more rows", rows.size() - visibleRows);
    }
  }
}

float BimMeasurementElevationAxisAngleDegrees(
    const container::renderer::BimMeasurementResult &measurement) {
  constexpr float kRadiansToDegrees = 57.29577951308232f;
  if (measurement.distance <= 0.0f) {
    return 0.0f;
  }
  return std::acos(std::clamp(std::abs(measurement.elevationDelta) /
                                  measurement.distance,
                              0.0f, 1.0f)) *
         kRadiansToDegrees;
}

ImVec4 BimSemanticLegendColor(container::renderer::BimSemanticColorMode mode,
                              size_t valueIndex) {
  static constexpr std::array<ImVec4, 14> kBimSemanticLegendPalette{{
      {0.16f, 0.47f, 0.86f, 1.0f},
      {0.86f, 0.29f, 0.24f, 1.0f},
      {0.13f, 0.58f, 0.39f, 1.0f},
      {0.88f, 0.58f, 0.16f, 1.0f},
      {0.55f, 0.36f, 0.80f, 1.0f},
      {0.10f, 0.65f, 0.72f, 1.0f},
      {0.82f, 0.33f, 0.58f, 1.0f},
      {0.45f, 0.52f, 0.22f, 1.0f},
      {0.93f, 0.42f, 0.18f, 1.0f},
      {0.22f, 0.62f, 0.93f, 1.0f},
      {0.62f, 0.43f, 0.25f, 1.0f},
      {0.42f, 0.46f, 0.58f, 1.0f},
      {0.70f, 0.28f, 0.42f, 1.0f},
      {0.34f, 0.68f, 0.51f, 1.0f},
  }};

  const uint32_t semanticId = static_cast<uint32_t>(
      std::min<size_t>(valueIndex + 1u, std::numeric_limits<uint32_t>::max()));
  const uint32_t paletteIndex =
      (semanticId - 1u + static_cast<uint32_t>(mode) * 3u) %
      static_cast<uint32_t>(kBimSemanticLegendPalette.size());
  return kBimSemanticLegendPalette[paletteIndex];
}

std::span<const std::string>
BimSemanticLegendValues(container::renderer::BimSemanticColorMode mode,
                        const BimInspectionState &inspection) {
  switch (mode) {
  case container::renderer::BimSemanticColorMode::Type:
    return inspection.elementTypes;
  case container::renderer::BimSemanticColorMode::Storey:
    return inspection.elementStoreys;
  case container::renderer::BimSemanticColorMode::Material:
    return inspection.elementMaterials;
  case container::renderer::BimSemanticColorMode::FireRating:
    return inspection.elementFireRatings;
  case container::renderer::BimSemanticColorMode::LoadBearing:
    return inspection.elementLoadBearingValues;
  case container::renderer::BimSemanticColorMode::Status:
    return inspection.elementStatuses;
  case container::renderer::BimSemanticColorMode::Off:
    break;
  }
  return {};
}

std::string
BimSemanticSelectionValue(container::renderer::BimSemanticColorMode mode,
                          const BimInspectionState &inspection) {
  switch (mode) {
  case container::renderer::BimSemanticColorMode::Type:
    return inspection.type;
  case container::renderer::BimSemanticColorMode::Storey:
    return !inspection.storeyName.empty() ? inspection.storeyName
                                          : inspection.storeyId;
  case container::renderer::BimSemanticColorMode::Material:
    return !inspection.materialName.empty() ? inspection.materialName
                                            : inspection.materialCategory;
  case container::renderer::BimSemanticColorMode::FireRating:
    return inspection.fireRating;
  case container::renderer::BimSemanticColorMode::LoadBearing:
    return inspection.loadBearing;
  case container::renderer::BimSemanticColorMode::Status:
    return inspection.status;
  case container::renderer::BimSemanticColorMode::Off:
    break;
  }
  return {};
}

std::optional<size_t> FindStringIndex(std::span<const std::string> values,
                                      const std::string &value) {
  if (value.empty()) {
    return std::nullopt;
  }
  const auto it = std::ranges::find(values, value);
  if (it == values.end()) {
    return std::nullopt;
  }
  return static_cast<size_t>(std::distance(values.begin(), it));
}

bool ContainsCaseInsensitive(std::string_view haystack,
                             std::string_view needle) {
  if (needle.empty()) {
    return true;
  }
  return ToLowerAscii(std::string(haystack))
             .find(ToLowerAscii(std::string(needle))) != std::string::npos;
}

const char *
BimDisciplinePresetLabel(container::renderer::BimDisciplinePreset preset) {
  switch (preset) {
  case container::renderer::BimDisciplinePreset::None:
    return "No preset";
  case container::renderer::BimDisciplinePreset::Architecture:
    return "Architecture";
  case container::renderer::BimDisciplinePreset::MepXray:
    return "MEP x-ray";
  }
  return "No preset";
}

bool PropertyMatchesSearch(
    const container::renderer::BimElementProperty &property,
    std::string_view search) {
  return ContainsCaseInsensitive(property.set, search) ||
         ContainsCaseInsensitive(property.name, search) ||
         ContainsCaseInsensitive(property.value, search) ||
         ContainsCaseInsensitive(property.category, search);
}

std::string RelationshipNodeLabel(
    const container::renderer::BimRelationshipNode &node) {
  if (!node.label.empty()) {
    return node.label;
  }
  if (!node.guid.empty()) {
    return node.guid;
  }
  if (!node.sourceId.empty()) {
    return node.sourceId;
  }
  if (node.objectIndex != std::numeric_limits<uint32_t>::max()) {
    return "Object " + std::to_string(node.objectIndex);
  }
  return "(unnamed)";
}

void DrawSemanticLegendEntry(container::renderer::BimSemanticColorMode mode,
                             size_t valueIndex, const std::string &label) {
  ImGui::PushID(static_cast<int>(valueIndex));
  const ImVec4 color = BimSemanticLegendColor(mode, valueIndex);
  ImGui::ColorButton("##SemanticLegendSwatch", color,
                     ImGuiColorEditFlags_NoTooltip |
                         ImGuiColorEditFlags_NoDragDrop,
                     ImVec2(16.0f, 16.0f));
  ImGui::SameLine();
  ImGui::TextWrapped("%s", label.c_str());
  ImGui::PopID();
}

} // namespace

std::optional<BimMeasurementCapturedPoint>
CaptureBimMeasurementPointFromSelection(
    const BimInspectionState &inspection,
    const BimMeasurementSnapUiState &snapState) {
  return captureBimMeasurementPointFromSelection(inspection, snapState);
}

GuiManager::~GuiManager() = default;

std::span<const container::renderer::BimCoordinationOverlayIssuePin>
GuiManager::bimIssueOverlayPins() const {
  return std::span<const container::renderer::BimCoordinationOverlayIssuePin>(
      bcfIssueOverlayPins_.data(), bcfIssueOverlayPins_.size());
}

void GuiManager::setSectionPlaneState(const SectionPlaneState &state) {
  sectionPlaneState_ = state;
  const float normalLength = glm::length(sectionPlaneState_.normal);
  sectionPlaneState_.normal =
      normalLength > 0.0001f ? sectionPlaneState_.normal / normalLength
                             : glm::vec3{0.0f, 1.0f, 0.0f};
  sectionPlaneState_.visualPlaneSize =
      std::clamp(sectionPlaneState_.visualPlaneSize, 0.1f, 100000.0f);
  sectionPlaneState_.visualPlaneOpacity =
      std::clamp(sectionPlaneState_.visualPlaneOpacity, 0.05f, 1.0f);
  sectionPlaneState_.visualPlaneLineWidth =
      std::clamp(sectionPlaneState_.visualPlaneLineWidth, 0.5f, 8.0f);
  sectionPlaneAxis_ = -1;
}

void GuiManager::setSectionPlaneVisualEditable(bool editable) {
  sectionPlaneState_.visualPlaneEditable =
      editable && sectionPlaneState_.enabled &&
      sectionPlaneState_.visualPlaneVisible;
}

void GuiManager::applyBimElevationDisplayIntent() {
  switch (bimElevationViewState_.style) {
  case BimElevationTechnicalStyle::Shaded:
    gBufferViewMode_ = GBufferViewMode::Overview;
    wireframeSettings_.enabled = false;
    break;
  case BimElevationTechnicalStyle::ShadedWithLines:
    gBufferViewMode_ = GBufferViewMode::Overview;
    wireframeSettings_.enabled = wireframeSupported_;
    wireframeSettings_.mode = WireframeMode::Overlay;
    wireframeSettings_.depthTest = bimElevationViewState_.useDepthTestedLines;
    wireframeSettings_.color = {0.02f, 0.02f, 0.02f};
    wireframeSettings_.overlayIntensity = 0.70f;
    wireframeSettings_.lineWidth = wireframeWideLineSupported_ ? 1.25f : 1.0f;
    break;
  case BimElevationTechnicalStyle::HiddenLine:
    gBufferViewMode_ = GBufferViewMode::Albedo;
    wireframeSettings_.enabled = wireframeSupported_;
    wireframeSettings_.mode = WireframeMode::Full;
    wireframeSettings_.depthTest = true;
    wireframeSettings_.color = {0.02f, 0.02f, 0.02f};
    wireframeSettings_.overlayIntensity = 1.0f;
    wireframeSettings_.lineWidth = wireframeWideLineSupported_ ? 1.5f : 1.0f;
    break;
  }
}

void GuiManager::initialize(VkInstance instance, VkDevice device,
                            VkPhysicalDevice physicalDevice,
                            VkQueue graphicsQueue, uint32_t graphicsQueueFamily,
                            RenderingPassHandle renderPass, uint32_t imageCount,
                            GLFWwindow *window,
                            const std::string &defaultModelPath,
                            float defaultImportScale) {
  if (initialized_)
    return;

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGui::StyleColorsDark();

  ImGui_ImplGlfw_InitForVulkan(window, true);

  std::array<VkDescriptorPoolSize, 11> poolSizes = {{
      {VK_DESCRIPTOR_TYPE_SAMPLER, 1000},
      {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1000},
      {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1000},
      {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1000},
      {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1000},
      {VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 1000},
      {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1000},
      {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1000},
      {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1000},
      {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1000},
      {VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 1000},
  }};

  VkDescriptorPoolCreateInfo poolInfo{};
  poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
  poolInfo.maxSets = 1000 * static_cast<uint32_t>(poolSizes.size());
  poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
  poolInfo.pPoolSizes = poolSizes.data();

  if (createOwnedDescriptorPool(device, &poolInfo, nullptr, &descriptorPool_) !=
      VK_SUCCESS) {
    throw std::runtime_error("Failed to create ImGui descriptor pool");
  }

  ImGui_ImplVulkan_InitInfo initInfo{};
  initInfo.Instance = instance;
  initInfo.PhysicalDevice = physicalDevice;
  initInfo.Device = device;
  initInfo.QueueFamily = graphicsQueueFamily;
  initInfo.Queue = graphicsQueue;
  initInfo.DescriptorPool = descriptorPool_;
  initInfo.UseDynamicRendering = true;
  initInfo.PipelineRenderingCreateInfo = renderPass->pipelineInfo();
  initInfo.Subpass = 0;
  initInfo.MinImageCount = imageCount;
  initInfo.ImageCount = imageCount;
  initInfo.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
  initInfo.CheckVkResultFn = CheckVkResult;

  ImGui_ImplVulkan_Init(&initInfo);
  ImGui_ImplVulkan_CreateFontsTexture();

  defaultModelPath_ = defaultModelPath;
  modelPathInput_ = defaultModelPath;
  const auto importScaleIt =
      std::ranges::find(kImportScaleValues, defaultImportScale);
  importScaleIndex_ =
      importScaleIt != kImportScaleValues.end()
          ? static_cast<int>(importScaleIt - kImportScaleValues.begin())
          : 0;
  importScale_ = kImportScaleValues[static_cast<size_t>(importScaleIndex_)];
  discoverSampleModels();
  initialized_ = true;
}

void GuiManager::discoverSampleModels() {
  sampleModelOptions_.clear();
  selectedSampleModelIndex_ = -1;
  std::unordered_set<std::string> knownModelPaths;

  const auto appendOption = [&](std::string label,
                                const std::filesystem::path &path) {
    if (!knownModelPaths.insert(ModelPathKey(path)).second) {
      return;
    }

    SampleModelOption option{};
    option.label = std::move(label);
    option.path = container::util::pathToUtf8(path);
    sampleModelOptions_.push_back(std::move(option));
  };

  const auto sampleRoot = ResolveSampleModelsRoot();
  if (sampleRoot) {
    std::error_code error;
    for (std::filesystem::directory_iterator it(*sampleRoot, error), end;
         !error && it != end; it.increment(error)) {
      std::error_code directoryError;
      if (!it->is_directory(directoryError)) {
        continue;
      }

      std::string selectedVariant;
      const auto modelPath =
          PreferredSampleModelPath(it->path(), selectedVariant);
      if (!modelPath) {
        continue;
      }

      std::string label = container::util::pathToUtf8(it->path().filename());
      if (!selectedVariant.empty() && selectedVariant != "glTF") {
        label += " (" + selectedVariant + ")";
      }
      appendOption(std::move(label), *modelPath);
    }
  }

  for (const KnownSampleAsset &asset : kKnownAuxiliarySampleAssets) {
    const auto modelPath = ResolveRelativeAssetPath(asset.relativePath);
    if (!modelPath) {
      continue;
    }

    appendOption(asset.label, *modelPath);
  }
  for (const DiscoveredSampleAsset &asset : DiscoverAuxiliarySampleAssets()) {
    appendOption(asset.label, asset.path);
  }

  std::ranges::sort(sampleModelOptions_, {}, &SampleModelOption::label);
  selectedSampleModelIndex_ = sampleModelIndexForPath(modelPathInput_);
}

int GuiManager::sampleModelIndexForPath(const std::string &path) const {
  if (path.empty()) {
    return -1;
  }

  const std::string target = ModelPathKey(container::util::pathFromUtf8(path));
  for (size_t i = 0; i < sampleModelOptions_.size(); ++i) {
    if (ModelPathKey(container::util::pathFromUtf8(
            sampleModelOptions_[i].path)) == target) {
      return static_cast<int>(i);
    }
  }

  return -1;
}

void GuiManager::queueModelLoadRequest(std::string path, float importScale,
                                       std::string label) {
  pendingModelLoadRequest_ = ModelLoadRequest{.path = std::move(path),
                                              .importScale = importScale,
                                              .label = std::move(label)};
}

bool GuiManager::queueSampleModelLoadRequest(std::string_view label) {
  const auto found =
      std::ranges::find(sampleModelOptions_, label, &SampleModelOption::label);
  if (found == sampleModelOptions_.end())
    return false;
  selectedSampleModelIndex_ =
      static_cast<int>(found - sampleModelOptions_.begin());
  modelPathInput_ = found->path;
  queueModelLoadRequest(modelPathInput_, importScale_, found->label);
  statusMessage_ = "Model load requested: " + found->label + " @ " +
                   ImportScaleLabel(importScale_);
  return true;
}

void GuiManager::shutdown(VkDevice device) {
  if (!initialized_)
    return;

  endFrame();
  ImGui_ImplVulkan_Shutdown();
  ImGui_ImplGlfw_Shutdown();
  ImGui::DestroyContext();

  if (descriptorPool_ != VK_NULL_HANDLE) {
    destroyOwnedDescriptorPool(device, descriptorPool_, nullptr);
    descriptorPool_ = VK_NULL_HANDLE;
  }

  initialized_ = false;
  imguiFrameOpen_ = false;
}

void GuiManager::updateSwapchainImageCount(uint32_t imageCount) {
  if (!initialized_)
    return;
  ImGui_ImplVulkan_SetMinImageCount(imageCount);
}

void GuiManager::setMsaaSampleState(std::span<const uint32_t> options,
                                    uint32_t activeSamples) {
  msaaSampleOptions_.assign(options.begin(), options.end());
  if (msaaSampleOptions_.empty()) {
    msaaSampleOptions_.push_back(1u);
  }
  msaaSamples_ = activeSamples == 0u ? 1u : activeSamples;
}

std::optional<uint32_t> GuiManager::consumeMsaaSampleChange() {
  auto requested = pendingMsaaSamples_;
  pendingMsaaSamples_.reset();
  return requested;
}

void GuiManager::setRenderEngineOptions(
    std::vector<RenderEngineOption> options,
    container::renderer::RenderTechniqueId active) {
  renderEngineOptions_ = std::move(options);
  activeRenderEngine_ = active;
  const bool pendingAvailable =
      pendingRenderEngineChange_.has_value() &&
      std::ranges::any_of(renderEngineOptions_,
                          [pending = *pendingRenderEngineChange_](
                              const RenderEngineOption& option) {
                            return option.available && option.id == pending;
                          });
  if (pendingRenderEngineChange_ == activeRenderEngine_ || !pendingAvailable) {
    pendingRenderEngineChange_.reset();
  }
}

std::optional<container::renderer::RenderTechniqueId>
GuiManager::consumeRenderEngineChange() {
  auto requested = pendingRenderEngineChange_;
  pendingRenderEngineChange_.reset();
  return requested;
}

container::renderer::RenderTechniqueId GuiManager::activeRenderEngine() const {
  return activeRenderEngine_;
}

void GuiManager::setRenderTechniqueDebugState(
    GuiRenderTechniqueDebugState state) {
  renderTechniqueDebugState_ = std::move(state);
  if (renderTechniqueDebugState_.displayModes.empty()) {
    return;
  }

  const uint32_t currentMode = static_cast<uint32_t>(gBufferViewMode_);
  const bool currentModeSupported =
      std::ranges::any_of(renderTechniqueDebugState_.displayModes,
                          [currentMode](
                              const GuiRenderTechniqueDisplayMode &mode) {
                            return mode.value == currentMode;
                          });
  if (!currentModeSupported) {
    gBufferViewMode_ = static_cast<GBufferViewMode>(
        renderTechniqueDebugState_.displayModes.front().value);
  }
}

const GuiRenderTechniqueDebugState &
GuiManager::renderTechniqueDebugState() const {
  return renderTechniqueDebugState_;
}

std::optional<ModelLoadRequest> GuiManager::consumeModelLoadRequest() {
  std::optional<ModelLoadRequest> request = std::move(pendingModelLoadRequest_);
  pendingModelLoadRequest_.reset();
  return request;
}

void GuiManager::startFrame() {
  if (!initialized_)
    return;
  endFrame();
  ImGui_ImplVulkan_NewFrame();
  ImGui_ImplGlfw_NewFrame();
  ImGui::NewFrame();
  imguiFrameOpen_ = true;
}

void GuiManager::endFrame() {
  if (!initialized_ || !imguiFrameOpen_)
    return;
  ImGui::EndFrame();
  imguiFrameOpen_ = false;
}

void GuiManager::render(VkCommandBuffer commandBuffer) {
  if (!initialized_ || !imguiFrameOpen_)
    return;
  ImGui::Render();
  imguiFrameOpen_ = false;
  ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), commandBuffer);
}

void GuiManager::drawViewportInteractionControls(
    const ViewportInteractionState &state,
    const std::function<void(ViewportTool)> &setTool,
    const std::function<void(TransformSpace)> &setTransformSpace,
    const std::function<void(TransformAxis)> &setTransformAxis,
    const std::function<void(ViewportNavigationStyle)> &setNavigationStyle,
    const std::function<void(bool)> &setTransformSnapEnabled) {
  if (!initialized_)
    return;

  static constexpr float kViewportControlsMinWidth = 260.0f;
  static constexpr float kViewportControlsMaxWidth = 420.0f;
  const ImGuiWindowFlags viewportWindowFlags =
      ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings;
  ImGui::SetNextWindowSizeConstraints(
      ImVec2(kViewportControlsMinWidth, 0.0f),
      ImVec2(kViewportControlsMaxWidth, std::numeric_limits<float>::max()));
  ImGui::Begin("Viewport", nullptr, viewportWindowFlags);
  ImGui::Text("Tool");
  if (ToolButton("Select", ViewportTool::Select, state.tool,
                 "Click visible geometry to select it.") &&
      setTool) {
    setTool(ViewportTool::Select);
  }
  ImGui::SameLine();
  if (ToolButton("Move", ViewportTool::Translate, state.tool,
                 "Drag a gizmo arrow or center box to move the selection.") &&
      setTool) {
    setTool(ViewportTool::Translate);
  }
  ImGui::SameLine();
  if (ToolButton("Rotate", ViewportTool::Rotate, state.tool,
                 "Drag a gizmo ring to rotate the selection.") &&
      setTool) {
    setTool(ViewportTool::Rotate);
  }
  ImGui::SameLine();
  if (ToolButton("Scale", ViewportTool::Scale, state.tool,
                 "Drag a gizmo box handle to scale the selection.") &&
      setTool) {
    setTool(ViewportTool::Scale);
  }

  int transformSpace = static_cast<int>(state.transformSpace);
  static constexpr const char *kTransformSpaceLabels[] = {"Local", "World"};
  if (ImGui::Combo("Space", &transformSpace, kTransformSpaceLabels,
                   IM_ARRAYSIZE(kTransformSpaceLabels)) &&
      setTransformSpace) {
    setTransformSpace(static_cast<TransformSpace>(transformSpace));
  }

  int transformAxis = static_cast<int>(state.transformAxis);
  static constexpr const char *kTransformAxisLabels[] = {"Free", "X", "Y", "Z"};
  if (ImGui::Combo("Axis", &transformAxis, kTransformAxisLabels,
                   IM_ARRAYSIZE(kTransformAxisLabels)) &&
      setTransformAxis) {
    setTransformAxis(static_cast<TransformAxis>(transformAxis));
  }

  int navigationStyle = static_cast<int>(state.navigationStyle);
  static constexpr const char *kNavigationStyleLabels[] = {"Revit", "Blender"};
  if (ImGui::Combo("Navigation", &navigationStyle, kNavigationStyleLabels,
                   IM_ARRAYSIZE(kNavigationStyleLabels)) &&
      setNavigationStyle) {
    setNavigationStyle(static_cast<ViewportNavigationStyle>(navigationStyle));
  }
  ShowItemTooltip(state.navigationStyle == ViewportNavigationStyle::Revit
                      ? "Revit: middle drag pans, Shift+middle orbits."
                      : "Blender: middle drag orbits, Shift+middle pans.");

  bool transformSnapEnabled = state.transformSnapEnabled;
  if (ImGui::Checkbox("Snap", &transformSnapEnabled) &&
      setTransformSnapEnabled) {
    setTransformSnapEnabled(transformSnapEnabled);
  }
  ShowItemTooltip("Snap Move to 0.25, Rotate to 15 deg, Scale to 0.1.");

  ImGui::Text("Gesture: %s", ViewportGestureLabel(state.gesture));
  ImGui::Text("Constraint: %s", TransformAxisLabel(state.transformAxis));
  if (state.hoverTransformAxis != TransformAxis::Free &&
      state.transformAxis == TransformAxis::Free) {
    ImGui::Text("Handle: %s", TransformAxisLabel(state.hoverTransformAxis));
  }
  ImGui::End();
}

void GuiManager::drawViewportNavigationOverlay(
    const ViewportNavigationState &state,
    const std::function<void(CameraViewPreset)> &setViewPreset,
    const std::function<void(float, float)> &freeRotate,
    const std::function<void(float, float)> &panView,
    const std::function<void()> &toggleProjectionMode) {
  if (!initialized_)
    return;

  const ImGuiViewport *viewport = ImGui::GetMainViewport();
  const ImVec2 widgetSize{172.0f, 214.0f};
  const ImVec2 windowPos{viewport->WorkPos.x + viewport->WorkSize.x -
                             widgetSize.x - 18.0f,
                         viewport->WorkPos.y + 18.0f};
  ImGui::SetNextWindowPos(windowPos, ImGuiCond_Always);
  ImGui::SetNextWindowSize(widgetSize, ImGuiCond_Always);
  ImGui::SetNextWindowBgAlpha(0.0f);

  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  const ImGuiWindowFlags flags =
      ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
      ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
      ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
      ImGuiWindowFlags_NoNav;
  if (ImGui::Begin("Viewport Navigation", nullptr, flags)) {
    ImDrawList *drawList = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 center{origin.x + widgetSize.x * 0.5f, origin.y + 126.0f};
    const bool orthographic =
        state.projectionMode == CameraProjectionMode::Orthographic;
    const glm::vec3 right = glm::normalize(
        glm::length(state.cameraRight) > 0.0001f ? state.cameraRight
                                                 : glm::vec3{1.0f, 0.0f, 0.0f});
    const glm::vec3 up = glm::normalize(glm::length(state.cameraUp) > 0.0001f
                                            ? state.cameraUp
                                            : glm::vec3{0.0f, 1.0f, 0.0f});
    const glm::vec3 forward =
        glm::normalize(glm::length(state.cameraForward) > 0.0001f
                           ? state.cameraForward
                           : glm::vec3{0.0f, 0.0f, -1.0f});
    const auto projectAxis = [&](const glm::vec3 &axis) {
      const float x = glm::dot(axis, right);
      const float y = -glm::dot(axis, up);
      const float length = std::sqrt(x * x + y * y);
      ImVec2 direction{0.0f, 0.0f};
      if (length > 0.0001f) {
        direction = {x / length, y / length};
      } else {
        direction = {0.0f, glm::dot(axis, forward) > 0.0f ? 1.0f : -1.0f};
      }
      const float radius = 20.0f + 48.0f * std::clamp(length, 0.0f, 1.0f);
      return ImVec2(center.x + direction.x * radius,
                    center.y + direction.y * radius);
    };

    struct AxisEndpoint {
      const char *id;
      const char *label;
      const char *tooltip;
      glm::vec3 axis;
      ImU32 color;
      CameraViewPreset preset;
      bool positive;
      float depth;
      ImVec2 position;
      float radius;
      bool hovered{false};
    };

    std::array<AxisEndpoint, 6> endpoints{{
        {"##nav-axis-x-pos",
         "X",
         "Right view (+X)",
         {1.0f, 0.0f, 0.0f},
         IM_COL32(255, 78, 91, 255),
         CameraViewPreset::Right,
         true,
         0.0f,
         {},
         15.0f},
        {"##nav-axis-x-neg",
         "",
         "Left view (-X)",
         {-1.0f, 0.0f, 0.0f},
         IM_COL32(255, 78, 91, 255),
         CameraViewPreset::Left,
         false,
         0.0f,
         {},
         13.0f},
        {"##nav-axis-y-pos",
         "Y",
         "Top view (+Y)",
         {0.0f, 1.0f, 0.0f},
         IM_COL32(126, 220, 56, 255),
         CameraViewPreset::Top,
         true,
         0.0f,
         {},
         15.0f},
        {"##nav-axis-y-neg",
         "",
         "Bottom view (-Y)",
         {0.0f, -1.0f, 0.0f},
         IM_COL32(126, 220, 56, 255),
         CameraViewPreset::Bottom,
         false,
         0.0f,
         {},
         13.0f},
        {"##nav-axis-z-pos",
         "Z",
         "Front view (+Z)",
         {0.0f, 0.0f, 1.0f},
         IM_COL32(67, 140, 255, 255),
         CameraViewPreset::Front,
         true,
         0.0f,
         {},
         15.0f},
        {"##nav-axis-z-neg",
         "",
         "Back view (-Z)",
         {0.0f, 0.0f, -1.0f},
         IM_COL32(67, 140, 255, 255),
         CameraViewPreset::Back,
         false,
         0.0f,
         {},
         13.0f},
    }};

    for (auto &endpoint : endpoints) {
      endpoint.depth = glm::dot(endpoint.axis, forward);
      endpoint.position = projectAxis(endpoint.axis);
    }

    std::ranges::sort(endpoints,
                      [](const AxisEndpoint &lhs, const AxisEndpoint &rhs) {
                        return lhs.depth > rhs.depth;
                      });

    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const auto distanceSquared = [](const ImVec2 &a, const ImVec2 &b) {
      const float dx = a.x - b.x;
      const float dy = a.y - b.y;
      return dx * dx + dy * dy;
    };
    const bool diskHovered =
        distanceSquared(mouse, center) <= 78.0f * 78.0f &&
        ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    bool axisHovered = false;
    for (const auto &endpoint : endpoints) {
      const float hoverRadius = endpoint.radius + 5.0f;
      axisHovered = axisHovered || distanceSquared(mouse, endpoint.position) <=
                                       hoverRadius * hoverRadius;
    }

    if (!axisHovered && diskHovered) {
      constexpr float kDiskDragRadius = 78.0f;
      ImGui::SetCursorScreenPos(
          ImVec2(center.x - kDiskDragRadius, center.y - kDiskDragRadius));
      ImGui::InvisibleButton(
          "##viewport-nav-disk-drag",
          ImVec2(kDiskDragRadius * 2.0f, kDiskDragRadius * 2.0f),
          ImGuiButtonFlags_MouseButtonLeft |
              ImGuiButtonFlags_MouseButtonMiddle |
              ImGuiButtonFlags_MouseButtonRight);
      if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
        ShowWrappedTooltip("Drag to rotate. Middle/right drag to pan.");
      }
      if ((ImGui::IsItemHovered() || ImGui::IsItemActive()) &&
          ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        viewportNavFreeRotateActive_ = true;
      }
      if ((ImGui::IsItemHovered() || ImGui::IsItemActive()) &&
          (ImGui::IsMouseClicked(ImGuiMouseButton_Middle) ||
           ImGui::IsMouseClicked(ImGuiMouseButton_Right))) {
        viewportNavPanActive_ = true;
      }
    }

    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
      viewportNavFreeRotateActive_ = false;
    }
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Middle) &&
        !ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
      viewportNavPanActive_ = false;
    }

    const ImVec2 navigationDragDelta = ImGui::GetIO().MouseDelta;
    if (viewportNavFreeRotateActive_ && freeRotate) {
      if (navigationDragDelta.x != 0.0f || navigationDragDelta.y != 0.0f) {
        freeRotate(navigationDragDelta.x, navigationDragDelta.y);
      }
    }
    if (viewportNavPanActive_ && panView) {
      if (navigationDragDelta.x != 0.0f || navigationDragDelta.y != 0.0f) {
        panView(navigationDragDelta.x, navigationDragDelta.y);
      }
    }

    if (diskHovered || axisHovered || viewportNavFreeRotateActive_ ||
        viewportNavPanActive_) {
      drawList->AddCircleFilled(center, 78.0f,
                                viewportNavPanActive_
                                    ? IM_COL32(220, 224, 230, 118)
                                    : IM_COL32(220, 224, 230, 86),
                                72);
    }
    drawList->AddCircle(center, 66.0f, IM_COL32(236, 240, 245, 116), 72, 1.7f);
    drawList->AddCircle(center, 4.0f, IM_COL32(238, 242, 247, 210), 16, 1.5f);

    const ImVec2 kViewportNavOptionsButtonSize{28.0f, 24.0f};
    ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(24, 27, 32, 168));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(44, 49, 58, 242));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(56, 64, 76, 255));
    ImGui::SetCursorScreenPos(
        ImVec2(origin.x + widgetSize.x - kViewportNavOptionsButtonSize.x -
                   8.0f,
               origin.y + 8.0f));
    if (ImGui::Button("...##viewport-nav-options-button",
                      kViewportNavOptionsButtonSize)) {
      ImGui::OpenPopup("##viewport-nav-options");
    }
    ShowItemTooltip("Navigation options");
    if (ImGui::BeginPopup("##viewport-nav-options")) {
      ImGui::TextDisabled("%s", orthographic ? "Orthographic" : "Perspective");
      if (ImGui::Selectable(orthographic ? "Perspective" : "Orthographic") &&
          toggleProjectionMode) {
        toggleProjectionMode();
      }
      ImGui::EndPopup();
    }
    ImGui::PopStyleColor(3);

    for (const auto &endpoint : endpoints) {
      drawList->AddLine(center, endpoint.position, endpoint.color, 2.2f);
    }

    const auto projectCubePoint = [&](const glm::vec3 &point) {
      constexpr float kCubeScale = 21.0f;
      const float depth = glm::dot(point, forward);
      const float projectionScale =
          orthographic ? 1.0f
                       : 1.0f / std::clamp(1.0f - depth * 0.18f, 0.74f, 1.26f);
      return ImVec2(
          center.x + glm::dot(point, right) * kCubeScale * projectionScale,
          center.y - glm::dot(point, up) * kCubeScale * projectionScale);
    };

    const std::array<glm::vec3, 8> cubeCorners{{
        {-1.0f, -1.0f, -1.0f},
        {1.0f, -1.0f, -1.0f},
        {1.0f, 1.0f, -1.0f},
        {-1.0f, 1.0f, -1.0f},
        {-1.0f, -1.0f, 1.0f},
        {1.0f, -1.0f, 1.0f},
        {1.0f, 1.0f, 1.0f},
        {-1.0f, 1.0f, 1.0f},
    }};

    struct CubeFace {
      std::array<uint32_t, 4> cornerIndices{};
      glm::vec3 normal{0.0f};
      const char *label{""};
      float visibility{0.0f};
      float depth{0.0f};
      bool visible{false};
    };

    std::array<CubeFace, 6> cubeFaces{{
        {{{0u, 1u, 2u, 3u}}, {0.0f, 0.0f, -1.0f}, "BACK"},
        {{{4u, 7u, 6u, 5u}}, {0.0f, 0.0f, 1.0f}, "FRONT"},
        {{{0u, 3u, 7u, 4u}}, {-1.0f, 0.0f, 0.0f}, "LEFT"},
        {{{1u, 5u, 6u, 2u}}, {1.0f, 0.0f, 0.0f}, "RIGHT"},
        {{{0u, 4u, 5u, 1u}}, {0.0f, -1.0f, 0.0f}, "BOTTOM"},
        {{{3u, 2u, 6u, 7u}}, {0.0f, 1.0f, 0.0f}, "TOP"},
    }};

    bool hasVisibleCubeFace = false;
    float closestFaceVisibility = 1.0f;
    size_t closestFaceIndex = 0u;
    for (size_t faceIndex = 0; faceIndex < cubeFaces.size(); ++faceIndex) {
      auto &face = cubeFaces[faceIndex];
      face.visibility = glm::dot(face.normal, forward);
      face.visible = face.visibility < -0.001f;
      hasVisibleCubeFace = hasVisibleCubeFace || face.visible;
      face.depth = 0.0f;
      for (const uint32_t cornerIndex : face.cornerIndices) {
        face.depth += glm::dot(cubeCorners[cornerIndex], forward);
      }
      face.depth *= 0.25f;
      if (face.visibility < closestFaceVisibility) {
        closestFaceVisibility = face.visibility;
        closestFaceIndex = faceIndex;
      }
    }
    if (!hasVisibleCubeFace) {
      cubeFaces[closestFaceIndex].visible = true;
    }

    std::ranges::sort(cubeFaces, [](const CubeFace &lhs, const CubeFace &rhs) {
      return lhs.depth > rhs.depth;
    });

    for (const auto &face : cubeFaces) {
      if (!face.visible) {
        continue;
      }
      std::array<ImVec2, 4> points{};
      ImVec2 faceCenter{0.0f, 0.0f};
      for (size_t corner = 0; corner < points.size(); ++corner) {
        points[corner] =
            projectCubePoint(cubeCorners[face.cornerIndices[corner]]);
        faceCenter.x += points[corner].x;
        faceCenter.y += points[corner].y;
      }
      faceCenter.x *= 0.25f;
      faceCenter.y *= 0.25f;

      const float shade =
          std::clamp(0.72f - face.visibility * 0.16f, 0.55f, 0.92f);
      const auto channel = [shade](float value) {
        return static_cast<int>(std::clamp(value * shade, 0.0f, 255.0f));
      };
      const ImU32 fill =
          IM_COL32(channel(118.0f), channel(128.0f), channel(142.0f), 226);
      drawList->AddQuadFilled(points[0], points[1], points[2], points[3], fill);
      drawList->AddQuad(points[0], points[1], points[2], points[3],
                        IM_COL32(232, 236, 242, 225), 1.2f);

      const ImVec2 textSize = ImGui::CalcTextSize(face.label);
      drawList->AddText(ImVec2(faceCenter.x - textSize.x * 0.5f,
                               faceCenter.y - textSize.y * 0.5f),
                        IM_COL32(42, 47, 56, 235), face.label);
    }

    for (auto &endpoint : endpoints) {
      const float hitRadius = endpoint.radius + 5.0f;
      ImGui::SetCursorScreenPos(ImVec2(endpoint.position.x - hitRadius,
                                       endpoint.position.y - hitRadius));
      ImGui::InvisibleButton(endpoint.id,
                             ImVec2(hitRadius * 2.0f, hitRadius * 2.0f));
      endpoint.hovered = ImGui::IsItemHovered();
      ShowItemTooltip(endpoint.tooltip);
      if (ImGui::IsItemClicked() && setViewPreset) {
        setViewPreset(endpoint.preset);
      }

      const float drawRadius =
          endpoint.radius + (endpoint.hovered ? 2.0f : 0.0f);
      if (endpoint.positive) {
        drawList->AddCircleFilled(endpoint.position, drawRadius, endpoint.color,
                                  32);
        if (endpoint.hovered) {
          drawList->AddCircle(endpoint.position, drawRadius + 3.0f,
                              IM_COL32(255, 255, 255, 240), 32, 2.0f);
        }
        const ImVec2 textSize = ImGui::CalcTextSize(endpoint.label);
        drawList->AddText(ImVec2(endpoint.position.x - textSize.x * 0.5f,
                                 endpoint.position.y - textSize.y * 0.5f),
                          endpoint.hovered ? IM_COL32(255, 255, 255, 255)
                                           : IM_COL32(8, 10, 14, 255),
                          endpoint.label);
      } else {
        drawList->AddCircleFilled(
            endpoint.position, drawRadius,
            IM_COL32(28, 32, 38, endpoint.hovered ? 170 : 82), 32);
        drawList->AddCircle(endpoint.position, drawRadius, endpoint.color, 32,
                            endpoint.hovered ? 2.8f : 2.0f);
      }
    }
  }
  ImGui::End();
  ImGui::PopStyleVar(2);
}

void GuiManager::drawSceneControls(
    const container::scene::SceneGraph &sceneGraph,
    const std::function<void(ScenePrimitiveKind)> &addScenePrimitive,
    const TransformControls &cameraTransform,
    const std::function<void(const TransformControls &)> &applyCameraTransform,
    const TransformControls &sceneTransform,
    const std::function<void(const TransformControls &)> &applySceneTransform,
    const glm::vec3 &directionalLightPosition,
    const container::gpu::LightingData &lightingData,
    const std::vector<container::gpu::PointLightData> &pointLights,
    const std::vector<EditableLightEntity> &editableLights,
    EditableLightId selectedEditableLight,
    const std::function<void(EditableLightId)> &selectEditableLight,
    const std::function<void(const EditableLightEntity &)> &updateEditableLight,
    const std::function<void(EditableLightType)> &addManualEditableLight,
    uint32_t selectedMeshNode, const BimInspectionState &bimInspection,
    const ViewpointSnapshotState &currentViewpoint,
    const std::function<bool(const ViewpointSnapshotState &)> &restoreViewpoint,
    uint32_t rootSceneNode,
    const std::function<void(uint32_t)> &selectMeshNode,
    const std::function<void(uint32_t)> &focusSceneNode,
    const std::function<void(uint32_t, bool)> &setSceneNodeVisible,
    const std::function<void(uint32_t, std::optional<uint32_t>)>
        &reparentSceneNode,
    const TransformControls &meshTransform,
    const std::function<void(uint32_t, const TransformControls &)>
        &applyMeshTransform) {
  if (!initialized_)
    return;

  ImGui::Begin("Scene Controls");
  if (ImGui::Combo("Import scale", &importScaleIndex_,
                   kImportScaleLabels.data(),
                   static_cast<int>(kImportScaleLabels.size()))) {
    importScaleIndex_ = std::clamp(
        importScaleIndex_, 0, static_cast<int>(kImportScaleValues.size()) - 1);
    importScale_ = kImportScaleValues[static_cast<size_t>(importScaleIndex_)];
    statusMessage_ = "Import scale set to " + ImportScaleLabel(importScale_) +
                     "; reload model to apply";
  }

  if (!sampleModelOptions_.empty()) {
    const bool hasSelection = selectedSampleModelIndex_ >= 0 &&
                              selectedSampleModelIndex_ <
                                  static_cast<int>(sampleModelOptions_.size());
    const char *preview =
        hasSelection
            ? sampleModelOptions_[selectedSampleModelIndex_].label.c_str()
            : "Select sample model";
    if (ImGui::BeginCombo("Sample model", preview)) {
      for (int i = 0; i < static_cast<int>(sampleModelOptions_.size()); ++i) {
        const auto &option = sampleModelOptions_[i];
        const bool selected = i == selectedSampleModelIndex_;
        if (ImGui::Selectable(option.label.c_str(), selected)) {
          (void)queueSampleModelLoadRequest(option.label);
        }
        ShowItemTooltip(option.path);
        if (selected) {
          ImGui::SetItemDefaultFocus();
        }
      }
      ImGui::EndCombo();
    }
  } else {
    ImGui::TextDisabled("No sample models found");
  }

  ImGui::InputText("Model path", &modelPathInput_);

  if (ImGui::Button("Load model")) {
    selectedSampleModelIndex_ = sampleModelIndexForPath(modelPathInput_);
    queueModelLoadRequest(modelPathInput_, importScale_, modelPathInput_);
    statusMessage_ = "Model load requested: " + modelPathInput_ + " @ " +
                     ImportScaleLabel(importScale_);
  }

  ImGui::SameLine();

  if (ImGui::Button("Reload Default")) {
    modelPathInput_ = defaultModelPath_;
    selectedSampleModelIndex_ = sampleModelIndexForPath(modelPathInput_);
    queueModelLoadRequest(modelPathInput_, importScale_, "default model");
    statusMessage_ = "Model load requested: default model @ " +
                     ImportScaleLabel(importScale_);
  }

  if (ImGui::Button("Add Primitive")) {
    ImGui::OpenPopup("Add Primitive Menu");
  }
  if (ImGui::BeginPopup("Add Primitive Menu")) {
    if (ImGui::MenuItem("Quad")) {
      addScenePrimitive(ScenePrimitiveKind::Quad);
    }
    if (ImGui::MenuItem("Plane (double-sided wall)")) {
      addScenePrimitive(ScenePrimitiveKind::Plane);
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Cube")) {
      addScenePrimitive(ScenePrimitiveKind::Cube);
    }
    if (ImGui::MenuItem("Sphere")) {
      addScenePrimitive(ScenePrimitiveKind::Sphere);
    }
    if (ImGui::MenuItem("Triangular Prism")) {
      addScenePrimitive(ScenePrimitiveKind::TriangularPrism);
    }
    if (ImGui::MenuItem("Pyramid")) {
      addScenePrimitive(ScenePrimitiveKind::Pyramid);
    }
    if (ImGui::MenuItem("Torus")) {
      addScenePrimitive(ScenePrimitiveKind::Torus);
    }
    ImGui::EndPopup();
  }

  ImGui::Separator();
  if (!renderEngineOptions_.empty()) {
    std::string renderEnginePreview{
        container::renderer::renderTechniqueDisplayName(activeRenderEngine_)};
    for (const RenderEngineOption &option : renderEngineOptions_) {
      if (option.id == activeRenderEngine_) {
        renderEnginePreview = option.label;
        break;
      }
    }
    if (ImGui::BeginCombo("Rendering Engine", renderEnginePreview.c_str())) {
      for (const RenderEngineOption &option : renderEngineOptions_) {
        const bool selected = option.id == activeRenderEngine_;
        ImGui::BeginDisabled(!option.available);
        if (ImGui::Selectable(option.label.c_str(), selected) &&
            option.available && !selected) {
          pendingRenderEngineChange_ = option.id;
          statusMessage_ = "Rendering engine change pending: " + option.label;
        }
        ImGui::EndDisabled();
        if (!option.available && !option.unavailableReason.empty()) {
          ShowItemTooltip(option.unavailableReason,
                          ImGuiHoveredFlags_AllowWhenDisabled);
        }
        if (selected) {
          ImGui::SetItemDefaultFocus();
        }
      }
      ImGui::EndCombo();
    }
  }

  std::vector<GBufferViewMode> displayModeValues;
  std::vector<const char *> displayModeLabels;
  if (!renderTechniqueDebugState_.displayModes.empty()) {
    displayModeValues.reserve(renderTechniqueDebugState_.displayModes.size());
    displayModeLabels.reserve(renderTechniqueDebugState_.displayModes.size());
    for (const GuiRenderTechniqueDisplayMode &mode :
         renderTechniqueDebugState_.displayModes) {
      displayModeValues.push_back(static_cast<GBufferViewMode>(mode.value));
      displayModeLabels.push_back(mode.label.c_str());
    }
  } else {
    displayModeValues.reserve(kDisplayModeDefinitions.size());
    displayModeLabels.reserve(kDisplayModeDefinitions.size());
    for (const DisplayModeDefinition &mode : kDisplayModeDefinitions) {
      displayModeValues.push_back(mode.mode);
      displayModeLabels.push_back(mode.label);
    }
  }
  int displayModeIndex = 0;
  for (size_t i = 0; i < displayModeValues.size(); ++i) {
    if (displayModeValues[i] == gBufferViewMode_) {
      displayModeIndex = static_cast<int>(i);
      break;
    }
  }
  if (!displayModeLabels.empty() &&
      ImGui::Combo("Display", &displayModeIndex, displayModeLabels.data(),
                   static_cast<int>(displayModeLabels.size()))) {
    gBufferViewMode_ =
        displayModeValues[static_cast<size_t>(displayModeIndex)];
  }
  static constexpr const char *kSceneViewportModeLabels[] = {"Editor",
                                                             "Render Preview"};
  int sceneViewportMode = static_cast<int>(sceneViewportMode_);
  if (ImGui::Combo("Viewport Mode", &sceneViewportMode,
                   kSceneViewportModeLabels,
                   IM_ARRAYSIZE(kSceneViewportModeLabels))) {
    sceneViewportMode_ = static_cast<SceneViewportMode>(sceneViewportMode);
  }
  std::vector<std::string> msaaLabels;
  std::vector<const char *> msaaLabelPtrs;
  msaaLabels.reserve(msaaSampleOptions_.size());
  msaaLabelPtrs.reserve(msaaSampleOptions_.size());
  int msaaIndex = 0;
  for (size_t i = 0; i < msaaSampleOptions_.size(); ++i) {
    if (msaaSampleOptions_[i] == msaaSamples_) {
      msaaIndex = static_cast<int>(i);
    }
    msaaLabels.push_back(std::to_string(msaaSampleOptions_[i]) + "x");
  }
  for (const std::string &label : msaaLabels) {
    msaaLabelPtrs.push_back(label.c_str());
  }
  if (!msaaLabelPtrs.empty() &&
      ImGui::Combo("MSAA", &msaaIndex, msaaLabelPtrs.data(),
                   static_cast<int>(msaaLabelPtrs.size()))) {
    const uint32_t requestedSamples =
        msaaSampleOptions_[static_cast<size_t>(msaaIndex)];
    if (requestedSamples != msaaSamples_) {
      pendingMsaaSamples_ = requestedSamples;
      statusMessage_ = "MSAA change pending: " +
                       std::to_string(requestedSamples) + "x";
    }
  }

  ImGui::BeginDisabled(!editorOverlaysEnabled());
  ImGui::Checkbox("Overlay vertices", &showGeometryOverlay_);
  ImGui::Checkbox("Overlay lights", &showLightGizmos_);
  ImGui::Checkbox("Normal validation", &normalValidationSettings_.enabled);
  if (normalValidationSettings_.enabled) {
    ImGui::Checkbox("Normal validation face fill",
                    &normalValidationSettings_.showFaceFill);
    ImGui::SliderFloat("Normal line length",
                       &normalValidationSettings_.lineLength, 0.01f, 100.0f);
    ImGui::SliderFloat("Normal line offset",
                       &normalValidationSettings_.lineOffset, 0.0f, 0.05f);
    ImGui::SliderFloat("Normal face alpha",
                       &normalValidationSettings_.faceAlpha, 0.0f, 1.0f);
    if (wireframeWideLineSupported_) {
      ImGui::SliderFloat("Normal line width",
                         &normalValidationSettings_.lineWidth, 1.0f, 100.0f);
    } else {
      normalValidationSettings_.lineWidth = 1.0f;
      ImGui::BeginDisabled();
      ImGui::SliderFloat("Normal line width",
                         &normalValidationSettings_.lineWidth, 1.0f, 1.0f);
      ImGui::EndDisabled();
    }
  }

  ImGui::Separator();
  ImGui::Text("Wireframe Debug");
  ImGui::TextDisabled("Backend: %s", wireframeRasterModeSupported_
                                         ? "Native raster line"
                                         : "Shader fallback");
  if (!wireframeSupported_) {
    ImGui::BeginDisabled();
  }
  ImGui::Checkbox("Wireframe Enabled", &wireframeSettings_.enabled);
  int wireframeMode = static_cast<int>(wireframeSettings_.mode);
  static constexpr const char *kWireframeModeLabels[] = {"Overlay", "Full"};
  if (ImGui::Combo("Wireframe Mode", &wireframeMode, kWireframeModeLabels,
                   IM_ARRAYSIZE(kWireframeModeLabels))) {
    wireframeSettings_.mode = static_cast<WireframeMode>(wireframeMode);
  }
  ImGui::Checkbox("Wireframe Depth Test", &wireframeSettings_.depthTest);
  ImGui::ColorEdit3("Wireframe Color", &wireframeSettings_.color.x);
  ImGui::SliderFloat("Wireframe Intensity",
                     &wireframeSettings_.overlayIntensity, 0.0f, 1.0f);
  if (wireframeWideLineSupported_) {
    ImGui::SliderFloat("Wireframe Line Width", &wireframeSettings_.lineWidth,
                       1.0f, 8.0f);
  } else {
    wireframeSettings_.lineWidth = 1.0f;
    ImGui::BeginDisabled();
    ImGui::SliderFloat("Wireframe Line Width", &wireframeSettings_.lineWidth,
                       1.0f, 1.0f);
    ImGui::EndDisabled();
  }
  if (!wireframeSupported_) {
    wireframeSettings_.enabled = false;
    ImGui::EndDisabled();
    ImGui::TextDisabled("Wireframe unavailable: fillModeNonSolid unsupported");
  }
  ImGui::EndDisabled();
  ImGui::Text("Scene nodes: %zu", sceneGraph.nodeCount());
  ImGui::Text("Renderable primitives: %zu",
              sceneGraph.renderableNodes().size());

  DrawSceneHierarchyPanel(
      sceneGraph,
      SceneHierarchyPanelActions{.rootSceneNode = rootSceneNode,
                                 .selectedMeshNode = selectedMeshNode,
                                 .selectSceneNode = &selectMeshNode,
                                 .focusSceneNode = &focusSceneNode,
                                 .setSceneNodeVisible = &setSceneNodeVisible,
                                 .reparentSceneNode = &reparentSceneNode},
      sceneHierarchyParentCandidateCache_);

  if (!bimInspection.hasScene) {
    bimMeasurementModelPath_.clear();
    bimMeasurementEffectiveImportScale_ = 1.0f;
    bimMeasurementObjectCount_ = 0u;
    bimMeasurementPointA_ = {};
    bimMeasurementPointB_ = {};
    bimMeasurementPointC_ = {};
    bimMeasurementAnnotations_.clear();
    nextBimMeasurementAnnotationId_ = 1u;
    selectedBimMeasurementAnnotationIndex_ = -1;
    selectedBimStoreyRangeIndex_ = -1;
    bimSelectionSets_.clear();
    selectedBimSelectionSetIndex_ = -1;
    nextBimSelectionSetId_ = 1u;
  } else {
    const float effectiveScale =
        bimInspection.hasEffectiveImportScale
            ? bimInspection.effectiveImportScale
            : (bimInspection.hasImportScale ? bimInspection.importScale : 1.0f);
    const bool measurementSourceChanged =
        bimMeasurementModelPath_ != bimInspection.modelPath ||
        std::abs(bimMeasurementEffectiveImportScale_ - effectiveScale) >
            0.000001f ||
        bimMeasurementObjectCount_ != bimInspection.objectCount;
    if (measurementSourceChanged) {
      bimMeasurementModelPath_ = bimInspection.modelPath;
      bimMeasurementEffectiveImportScale_ = effectiveScale;
      bimMeasurementObjectCount_ = bimInspection.objectCount;
      bimMeasurementPointA_ = {};
      bimMeasurementPointB_ = {};
      bimMeasurementPointC_ = {};
      bimMeasurementAnnotations_.clear();
      nextBimMeasurementAnnotationId_ = 1u;
      selectedBimMeasurementAnnotationIndex_ = -1;
      selectedBimStoreyRangeIndex_ = -1;
      bimSelectionSets_.clear();
      selectedBimSelectionSetIndex_ = -1;
      nextBimSelectionSetId_ = 1u;
    }
  }

  if (bimInspection.hasScene) {
    ImGui::Separator();
    if (ImGui::TreeNode("BIM")) {
      ImGui::Text("Objects: %zu", bimInspection.objectCount);
      ImGui::Text("Objects by geometry: %zu mesh, %zu point-cloud, %zu curve",
                  bimInspection.meshObjectCount, bimInspection.pointObjectCount,
                  bimInspection.curveObjectCount);
      ImGui::Text("Mesh draws: %zu opaque, %zu transparent",
                  bimInspection.opaqueDrawCount,
                  bimInspection.transparentDrawCount);
      ImGui::Text("Point-cloud draws: %zu opaque, %zu transparent",
                  bimInspection.pointOpaqueDrawCount,
                  bimInspection.pointTransparentDrawCount);
      ImGui::Text("Curve draws: %zu opaque, %zu transparent",
                  bimInspection.curveOpaqueDrawCount,
                  bimInspection.curveTransparentDrawCount);
      ImGui::Text("Native point draws: %zu opaque, %zu transparent",
                  bimInspection.nativePointOpaqueDrawCount,
                  bimInspection.nativePointTransparentDrawCount);
      ImGui::Text("Native curve draws: %zu opaque, %zu transparent",
                  bimInspection.nativeCurveOpaqueDrawCount,
                  bimInspection.nativeCurveTransparentDrawCount);
      ImGui::Text("Meshlet clusters: %zu (%zu source, %zu estimated)",
                  bimInspection.meshletClusterCount,
                  bimInspection.meshletSourceClusterCount,
                  bimInspection.meshletEstimatedClusterCount);
      ImGui::Text("Cluster references: %zu object refs, max LOD %u",
                  bimInspection.meshletObjectReferenceCount,
                  bimInspection.meshletMaxLodLevel);
      ImGui::Text("GPU meshlet residency: %zu objects, %zu clusters, %.2f MB, "
                  "compute %s%s",
                  bimInspection.meshletGpuResidentObjectCount,
                  bimInspection.meshletGpuResidentClusterCount,
                  static_cast<double>(bimInspection.meshletGpuBufferBytes) /
                      (1024.0 * 1024.0),
                  bimInspection.meshletGpuComputeReady ? "ready" : "offline",
                  bimInspection.meshletGpuDispatchPending ? ", pending" : "");
      ImGui::Text("Optimized metadata cacheable: %s",
                  bimInspection.optimizedModelMetadataCacheable ? "yes" : "no");
      ImGui::Text("Optimized metadata cache: %s",
                  !bimInspection.optimizedModelMetadataCacheStatus.empty()
                      ? bimInspection.optimizedModelMetadataCacheStatus.c_str()
                  : bimInspection.optimizedModelMetadataCacheHit ? "hit"
                  : bimInspection.optimizedModelMetadataCacheWritten
                      ? "written"
                      : "not written");
      ImGui::Text("Floor-plan overlays: %zu", bimInspection.floorPlanDrawCount);
      ImGui::Text("Types: %zu", bimInspection.uniqueTypeCount);
      ImGui::Text("Storeys: %zu", bimInspection.uniqueStoreyCount);
      ImGui::Text("Materials: %zu", bimInspection.uniqueMaterialCount);
      ImGui::Text("Disciplines: %zu", bimInspection.uniqueDisciplineCount);
      ImGui::Text("Phases: %zu", bimInspection.uniquePhaseCount);
      ImGui::Text("Fire ratings: %zu", bimInspection.uniqueFireRatingCount);
      ImGui::Text("Load-bearing values: %zu",
                  bimInspection.uniqueLoadBearingCount);
      ImGui::Text("Statuses: %zu", bimInspection.uniqueStatusCount);
      if (!bimInspection.modelPath.empty()) {
        TextWrappedPathValue("Source", bimInspection.modelPath);
      }
      if (const auto *report = bimInspection.importReport;
          report && report->completeness !=
                        container::geometry::ImportCompleteness::Unreported) {
        const bool complete = report->completeness ==
                              container::geometry::ImportCompleteness::Complete;
        if (!complete)
          ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.7f, 0.25f, 1.0f));
        ImGui::TextWrapped("%s", report->summary().c_str());
        if (!complete)
          ImGui::PopStyleColor();
        if (!report->diagnostics.empty() &&
            ImGui::TreeNode("Import warnings")) {
          for (const auto &[type, count] : report->representationWarnings)
            ImGui::Text("%s: %zu product/representation pairs", type.c_str(),
                        count);
          if (ImGui::BeginChild("Import warning details", ImVec2(0, 200),
                                ImGuiChildFlags_Borders,
                                ImGuiWindowFlags_HorizontalScrollbar)) {
            ImGuiListClipper clipper;
            clipper.Begin(static_cast<int>(report->diagnostics.size()));
            while (clipper.Step()) {
              for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                const auto &d = report->diagnostics[static_cast<size_t>(i)];
                ImGui::Text("%s #%u / product #%u (%s): %s",
                            d.representationType.c_str(), d.entityId,
                            d.productId, d.productGuid.c_str(),
                            d.reason.c_str());
              }
            }
          }
          ImGui::EndChild();
          ImGui::TreePop();
        }
      }
      if (bimInspection.hasSourceUnits) {
        ImGui::TextWrapped("Source units: %s",
                           bimInspection.sourceUnits.c_str());
      }
      if (bimInspection.hasMetersPerUnit) {
        ImGui::Text("Meters per unit: %.9g", bimInspection.metersPerUnit);
      }
      if (bimInspection.hasImportScale) {
        ImGui::Text("Import scale: %.9g", bimInspection.importScale);
      }
      if (bimInspection.hasEffectiveImportScale) {
        ImGui::Text("Effective import scale: %.9g",
                    bimInspection.effectiveImportScale);
      }
      if (bimInspection.hasSourceUpAxis) {
        ImGui::TextWrapped("Source up axis: %s",
                           bimInspection.sourceUpAxis.c_str());
      }
      if (bimInspection.hasCoordinateOffset) {
        ImGui::Text("Coordinate offset: %.3f, %.3f, %.3f",
                    bimInspection.coordinateOffset.x,
                    bimInspection.coordinateOffset.y,
                    bimInspection.coordinateOffset.z);
        if (!bimInspection.coordinateOffsetSource.empty()) {
          ImGui::TextWrapped("Offset source: %s",
                             bimInspection.coordinateOffsetSource.c_str());
        }
      }
      if (!bimInspection.crsName.empty() ||
          !bimInspection.crsAuthority.empty() ||
          !bimInspection.crsCode.empty()) {
        std::string crs = bimInspection.crsName;
        if (!bimInspection.crsAuthority.empty() ||
            !bimInspection.crsCode.empty()) {
          if (!crs.empty()) {
            crs += " ";
          }
          crs += "(" + bimInspection.crsAuthority;
          if (!bimInspection.crsAuthority.empty() &&
              !bimInspection.crsCode.empty()) {
            crs += ":";
          }
          crs += bimInspection.crsCode + ")";
        }
        ImGui::TextWrapped("CRS: %s", crs.c_str());
      }
      if (!bimInspection.mapConversionName.empty()) {
        ImGui::TextWrapped("Map conversion: %s",
                           bimInspection.mapConversionName.c_str());
      }
      if (ImGui::TreeNode("Georeference and Units")) {
        auto drawMetadataRow = [](const char *label, const std::string &value) {
          ImGui::TableNextRow();
          ImGui::TableNextColumn();
          ImGui::TextDisabled("%s", label);
          ImGui::TableNextColumn();
          if (value.empty()) {
            ImGui::TextDisabled("not exposed");
          } else {
            TextWrappedPathValue("", value);
          }
        };
        auto drawMetadataFloatRow = [](const char *label, bool present,
                                       float value, const char *format) {
          ImGui::TableNextRow();
          ImGui::TableNextColumn();
          ImGui::TextDisabled("%s", label);
          ImGui::TableNextColumn();
          if (present) {
            ImGui::Text(format, value);
          } else {
            ImGui::TextDisabled("not exposed");
          }
        };
        if (ImGui::BeginTable("BimGeoreferenceAndUnits", 2,
                              ImGuiTableFlags_BordersInnerV |
                                  ImGuiTableFlags_RowBg |
                                  ImGuiTableFlags_SizingStretchProp)) {
          drawMetadataRow("Source path", bimInspection.modelPath);
          drawMetadataRow("Source units", bimInspection.sourceUnits);
          drawMetadataFloatRow("Meters per unit",
                               bimInspection.hasMetersPerUnit,
                               bimInspection.metersPerUnit, "%.9g");
          drawMetadataFloatRow("Import scale", bimInspection.hasImportScale,
                               bimInspection.importScale, "%.9g");
          drawMetadataFloatRow("Effective import scale",
                               bimInspection.hasEffectiveImportScale,
                               bimInspection.effectiveImportScale, "%.9g");
          drawMetadataRow("Source up axis", bimInspection.sourceUpAxis);
          ImGui::TableNextRow();
          ImGui::TableNextColumn();
          ImGui::TextDisabled("Coordinate offset");
          ImGui::TableNextColumn();
          if (bimInspection.hasCoordinateOffset) {
            ImGui::Text("%.3f, %.3f, %.3f", bimInspection.coordinateOffset.x,
                        bimInspection.coordinateOffset.y,
                        bimInspection.coordinateOffset.z);
          } else {
            ImGui::TextDisabled("not exposed");
          }
          drawMetadataRow("Offset source",
                          bimInspection.coordinateOffsetSource);
          drawMetadataRow("CRS name", bimInspection.crsName);
          drawMetadataRow("CRS authority", bimInspection.crsAuthority);
          drawMetadataRow("CRS code", bimInspection.crsCode);
          drawMetadataRow("Map conversion", bimInspection.mapConversionName);
          ImGui::EndTable();
        }
        if (bimInspection.crsName.empty() &&
            bimInspection.crsAuthority.empty() &&
            bimInspection.crsCode.empty()) {
          ImGui::TextDisabled("No CRS metadata exposed");
        }
        if (bimInspection.hasSelectedCoordinateReadout) {
          ImGui::Separator();
          ImGui::TextDisabled("Selected coordinate readout");
          DrawBimCoordinateReadout(bimInspection.selectedCoordinateReadout);
        }
        if (bimInspection.hasOriginRebaseRecommendation) {
          const auto &recommendation =
              bimInspection.originRebaseRecommendation;
          ImGui::Separator();
          ImGui::Text(
              "Origin rebase recommended: %s",
              recommendation.recommended ? "yes" : "no");
          ImGui::Text("Largest project coordinate: %.3f",
                      recommendation.largestProjectCoordinateMagnitude);
          ImGui::Text("Survey offset magnitude: %.3f",
                      recommendation.surveyOffsetMagnitude);
        }
        ImGui::TreePop();
      }

      if (ImGui::TreeNode("Schedules and Quantities")) {
        ImGui::Text("By IFC class and storey");
        DrawBimScheduleRows("BimScheduleClassStoreyRows",
                            bimInspection.scheduleByClassAndStoreyRows);
        ImGui::Separator();
        ImGui::Text("By type and storey");
        DrawBimScheduleRows("BimScheduleTypeStoreyRows",
                            bimInspection.scheduleByTypeAndStoreyRows);
        ImGui::Separator();
        ImGui::Text("Material totals");
        DrawBimScheduleRows("BimScheduleMaterialTotals",
                            bimInspection.scheduleByMaterialRows);
        ImGui::TreePop();
      }

      if (ImGui::TreeNode("Model Compare")) {
        ImGui::Text("Current comparable elements: %zu",
                    bimInspection.modelCompareElements.size());
        if (ImGui::SmallButton("Capture compare baseline")) {
          bimCompareBaseline_.assign(bimInspection.modelCompareElements.begin(),
                                     bimInspection.modelCompareElements.end());
          bimCompareBaselineModelPath_ = bimInspection.modelPath;
          bimCompareChanges_.clear();
          statusMessage_ = "Captured BIM compare baseline";
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Compare current model")) {
          const auto result = container::renderer::compareBimModels(
              bimCompareBaseline_, bimInspection.modelCompareElements);
          bimCompareChanges_ = result.changes;
          statusMessage_ = "Compared current BIM model";
        }
        if (bimCompareBaseline_.empty()) {
          ImGui::TextDisabled("No compare baseline captured");
        } else {
          ImGui::Text("Baseline elements: %zu", bimCompareBaseline_.size());
          if (!bimCompareBaselineModelPath_.empty()) {
            TextWrappedPathValue("Baseline model", bimCompareBaselineModelPath_);
          }
          if (bimCompareChanges_.empty()) {
            ImGui::TextDisabled("No detected changes");
          } else if (ImGui::BeginTable("BimModelCompareChanges", 4,
                                       ImGuiTableFlags_BordersInnerV |
                                           ImGuiTableFlags_RowBg |
                                           ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Kind");
            ImGui::TableSetupColumn("Element");
            ImGui::TableSetupColumn("Before");
            ImGui::TableSetupColumn("After");
            ImGui::TableHeadersRow();
            const size_t visibleChanges =
                std::min<size_t>(bimCompareChanges_.size(), 32u);
            for (size_t i = 0; i < visibleChanges; ++i) {
              const auto &change = bimCompareChanges_[i];
              ImGui::TableNextRow();
              ImGui::TableNextColumn();
              ImGui::Text("%s", BimModelCompareChangeKindLabel(change.kind));
              ImGui::TableNextColumn();
              ImGui::TextWrapped("%s", change.identity.c_str());
              ImGui::TableNextColumn();
              ImGui::TextWrapped("%s", change.beforeValue.c_str());
              ImGui::TableNextColumn();
              ImGui::TextWrapped("%s", change.afterValue.c_str());
            }
            ImGui::EndTable();
          }
        }
        ImGui::TreePop();
      }

      const std::string semanticColorLabel(
          container::renderer::bimSemanticColorModeLabel(
              bimSemanticColorMode_));
      if (ImGui::BeginCombo("Semantic color", semanticColorLabel.c_str())) {
        for (container::renderer::BimSemanticColorMode mode :
             container::renderer::kBimSemanticColorModes) {
          const std::string label(
              container::renderer::bimSemanticColorModeLabel(mode));
          const bool selected = bimSemanticColorMode_ == mode;
          if (ImGui::Selectable(label.c_str(), selected)) {
            bimSemanticColorMode_ = mode;
          }
          if (selected) {
            ImGui::SetItemDefaultFocus();
          }
        }
        ImGui::EndCombo();
      }
      if (ImGui::TreeNode("Semantic color legend")) {
        if (bimSemanticColorMode_ ==
            container::renderer::BimSemanticColorMode::Off) {
          ImGui::TextDisabled(
              "Original materials do not have a generated semantic legend");
          if (bimInspection.hasSelection) {
            glm::vec4 color = bimInspection.sourceColor;
            ImGui::BeginDisabled();
            ImGui::ColorEdit4("Selected source color", &color.x);
            ImGui::EndDisabled();
          }
        } else {
          const std::span<const std::string> legendValues =
              BimSemanticLegendValues(bimSemanticColorMode_, bimInspection);
          if (bimInspection.hasSelection) {
            const std::string selectedSemanticValue =
                BimSemanticSelectionValue(bimSemanticColorMode_, bimInspection);
            ImGui::TextWrapped("Selected semantic value: %s",
                               selectedSemanticValue.empty()
                                   ? "not assigned"
                                   : selectedSemanticValue.c_str());
            if (const auto selectedLegendIndex =
                    FindStringIndex(legendValues, selectedSemanticValue)) {
              ImGui::ColorButton("Selected semantic color",
                                 BimSemanticLegendColor(bimSemanticColorMode_,
                                                        *selectedLegendIndex),
                                 ImGuiColorEditFlags_NoTooltip |
                                     ImGuiColorEditFlags_NoDragDrop,
                                 ImVec2(18.0f, 18.0f));
              ImGui::SameLine();
              ImGui::Text("Semantic ID: %u",
                          static_cast<uint32_t>(*selectedLegendIndex + 1u));
            } else if (!selectedSemanticValue.empty()) {
              ImGui::TextDisabled(
                  "Selected value is not in the exposed legend");
            }
          }
          if (legendValues.empty()) {
            ImGui::TextDisabled("No legend values exposed for this mode");
          } else {
            ImGui::TextDisabled("Palette matches the active BIM semantic mode");
            const size_t visibleLegendCount =
                std::min<size_t>(legendValues.size(), 48u);
            for (size_t i = 0; i < visibleLegendCount; ++i) {
              DrawSemanticLegendEntry(bimSemanticColorMode_, i,
                                      legendValues[i]);
            }
            if (visibleLegendCount < legendValues.size()) {
              ImGui::TextDisabled("%zu more legend values",
                                  legendValues.size() - visibleLegendCount);
            }
          }
        }
        ImGui::TreePop();
      }

      auto drawStringFilterCombo = [](const char *label, const char *allLabel,
                                      std::span<const std::string> values,
                                      bool &filterEnabled,
                                      std::string &filterValue) {
        if (values.empty()) {
          filterEnabled = false;
          filterValue.clear();
          return;
        }
        std::string currentLabel =
            filterEnabled && !filterValue.empty() ? filterValue : allLabel;
        const bool currentKnown =
            !filterEnabled || std::ranges::contains(values, filterValue);
        if (!currentKnown) {
          currentLabel = allLabel;
          filterEnabled = false;
          filterValue.clear();
        }
        if (ImGui::BeginCombo(label, currentLabel.c_str())) {
          const bool allSelected = !filterEnabled;
          if (ImGui::Selectable(allLabel, allSelected)) {
            filterEnabled = false;
            filterValue.clear();
          }
          if (allSelected) {
            ImGui::SetItemDefaultFocus();
          }
          for (const std::string &value : values) {
            const bool selected = filterEnabled && filterValue == value;
            if (ImGui::Selectable(value.c_str(), selected)) {
              filterEnabled = true;
              filterValue = value;
            }
            if (selected) {
              ImGui::SetItemDefaultFocus();
            }
          }
          ImGui::EndCombo();
        }
      };

      auto applyStoreyFilter =
          [&](const container::renderer::BimStoreyRange &storeyRange) {
            bimFilterState_.storeyFilterEnabled = true;
            bimFilterState_.storey = storeyRange.label;
          };
      auto applyStoreyPlanSlice =
          [&](const container::renderer::BimStoreyRange &storeyRange,
              bool enableFloorPlanOverlay) {
            const float height =
                storeyRange.maxElevation - storeyRange.minElevation;
            const float margin = std::max(0.05f, height * 0.03f);
            applyStoreyFilter(storeyRange);
            sectionPlaneState_.enabled = true;
            sectionPlaneAxis_ = 1;
            sectionPlaneState_.normal = {0.0f, -1.0f, 0.0f};
            sectionPlaneState_.offset = -(storeyRange.maxElevation + margin);
            if (enableFloorPlanOverlay) {
              bimFloorPlanOverlayState_.enabled = true;
              bimFloorPlanOverlayState_.elevationMode =
                  BimFloorPlanElevationMode::SourceElevation;
            }
          };
      auto clearStoreyPlanPreset = [&]() {
        bimFilterState_.storeyFilterEnabled = false;
        bimFilterState_.storey.clear();
        sectionPlaneState_ = {};
        sectionPlaneAxis_ = 1;
        selectedBimStoreyRangeIndex_ = -1;
      };
      auto floorSliceMatchesStorey =
          [&](const container::renderer::BimStoreyRange &storeyRange) {
            return sectionPlaneState_.enabled && sectionPlaneAxis_ == 1 &&
                   sectionPlaneState_.normal.y < 0.0f &&
                   bimFilterState_.storeyFilterEnabled &&
                   bimFilterState_.storey == storeyRange.label;
          };

      const auto elevationNormalForPreset = [](CameraViewPreset preset) {
        switch (preset) {
        case CameraViewPreset::Front:
          return glm::vec3{0.0f, 0.0f, -1.0f};
        case CameraViewPreset::Back:
          return glm::vec3{0.0f, 0.0f, 1.0f};
        case CameraViewPreset::Right:
          return glm::vec3{-1.0f, 0.0f, 0.0f};
        case CameraViewPreset::Left:
          return glm::vec3{1.0f, 0.0f, 0.0f};
        case CameraViewPreset::Top:
          return glm::vec3{0.0f, -1.0f, 0.0f};
        case CameraViewPreset::Bottom:
          return glm::vec3{0.0f, 1.0f, 0.0f};
        }
        return glm::vec3{0.0f, 0.0f, -1.0f};
      };
      const auto requestElevationView = [&](CameraViewPreset preset) {
        bimElevationViewState_.preset = preset;
        applyBimElevationDisplayIntent();
        if (bimElevationViewState_.syncSectionPlaneToView) {
          sectionPlaneState_.enabled = true;
          sectionPlaneAxis_ = -1;
          sectionPlaneState_.normal = elevationNormalForPreset(preset);
        }
        bimElevationViewRequest_ = BimElevationViewRequest{
            .preset = preset,
            .style = bimElevationViewState_.style,
            .forceOrthographic = bimElevationViewState_.forceOrthographic,
            .syncSectionPlaneToView =
                bimElevationViewState_.syncSectionPlaneToView};
        statusMessage_ =
            std::string("Applied BIM ") + CameraViewPresetLabel(preset) +
            " elevation (" +
            BimElevationTechnicalStyleLabel(bimElevationViewState_.style) + ")";
      };

      if (ImGui::TreeNode("BIM Elevation View")) {
        static constexpr const char *kElevationViewLabels[] = {
            "Front elevation", "Back elevation", "Right elevation",
            "Left elevation"};
        int elevationPresetIndex = 0;
        switch (bimElevationViewState_.preset) {
        case CameraViewPreset::Back:
          elevationPresetIndex = 1;
          break;
        case CameraViewPreset::Right:
          elevationPresetIndex = 2;
          break;
        case CameraViewPreset::Left:
          elevationPresetIndex = 3;
          break;
        default:
          elevationPresetIndex = 0;
          break;
        }
        if (ImGui::Combo("Elevation view", &elevationPresetIndex,
                         kElevationViewLabels,
                         IM_ARRAYSIZE(kElevationViewLabels))) {
          static constexpr std::array kElevationPresets{
              CameraViewPreset::Front, CameraViewPreset::Back,
              CameraViewPreset::Right, CameraViewPreset::Left};
          bimElevationViewState_.preset = kElevationPresets[static_cast<size_t>(
              std::clamp(elevationPresetIndex, 0, 3))];
        }
        int elevationStyle = static_cast<int>(bimElevationViewState_.style);
        static constexpr const char *kElevationStyleLabels[] = {
            "Shaded", "Technical", "Hidden line"};
        if (ImGui::Combo("Technical style", &elevationStyle,
                         kElevationStyleLabels,
                         IM_ARRAYSIZE(kElevationStyleLabels))) {
          bimElevationViewState_.style =
              static_cast<BimElevationTechnicalStyle>(
                  std::clamp(elevationStyle, 0, 2));
        }
        ImGui::Checkbox("Force orthographic",
                        &bimElevationViewState_.forceOrthographic);
        ImGui::Checkbox("Depth-tested lines",
                        &bimElevationViewState_.useDepthTestedLines);
        ImGui::Checkbox("Align section plane to elevation",
                        &bimElevationViewState_.syncSectionPlaneToView);
        if (ImGui::Button("Apply elevation view")) {
          requestElevationView(bimElevationViewState_.preset);
        }
        if (ImGui::BeginTable("BimElevationQuickViews", 2,
                              ImGuiTableFlags_SizingStretchSame)) {
          ImGui::TableNextColumn();
          if (ImGui::SmallButton("Front elevation")) {
            requestElevationView(CameraViewPreset::Front);
          }
          ImGui::TableNextColumn();
          if (ImGui::SmallButton("Back elevation")) {
            requestElevationView(CameraViewPreset::Back);
          }
          ImGui::TableNextRow();
          ImGui::TableNextColumn();
          if (ImGui::SmallButton("Right elevation")) {
            requestElevationView(CameraViewPreset::Right);
          }
          ImGui::TableNextColumn();
          if (ImGui::SmallButton("Left elevation")) {
            requestElevationView(CameraViewPreset::Left);
          }
          ImGui::EndTable();
        }
        ImGui::Text(
            "Active style: %s, %s lines",
            BimElevationTechnicalStyleLabel(bimElevationViewState_.style),
            wireframeSettings_.depthTest ? "hidden" : "x-ray");
        ImGui::TreePop();
      }

      if (ImGui::TreeNode("BIM Drawing Export")) {
        ImGui::InputText("SVG path", &bimDrawingExportUiState_.path);
        ImGui::InputFloat("Paper width (mm)",
                          &bimDrawingExportUiState_.paperWidthMm, 1.0f,
                          10.0f, "%.1f");
        ImGui::InputFloat("Paper height (mm)",
                          &bimDrawingExportUiState_.paperHeightMm, 1.0f,
                          10.0f, "%.1f");
        ImGui::InputFloat("Model units / paper mm",
                          &bimDrawingExportUiState_.modelUnitsPerPaperMm,
                          1.0f, 10.0f, "%.2f");
        const bool canExport = !bimDrawingExportUiState_.path.empty();
        if (!canExport) {
          ImGui::BeginDisabled();
        }
        if (ImGui::Button("Export SVG")) {
          bimDrawingExportUiState_.exportRequested = true;
          statusMessage_ = "BIM drawing SVG export requested";
        }
        if (!canExport) {
          ImGui::EndDisabled();
        }
        ImGui::TreePop();
      }

      if (ImGui::TreeNode("Search and Filters")) {
        ImGui::InputText("Search filter values", &bimQuickFilterSearch_);
        if (ImGui::SmallButton("Clear search")) {
          bimQuickFilterSearch_.clear();
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear all BIM filters")) {
          bimFilterState_ = {};
          bimPhaseTimelineUiState_ = {};
          selectedBimStoreyRangeIndex_ = -1;
          statusMessage_ = "Cleared BIM filters";
        }
        ImGui::TextWrapped(
            "Active filters: %s%s%s%s%s%s%s%s%s%s%s",
            bimFilterState_.typeFilterEnabled ? "type " : "",
            bimFilterState_.storeyFilterEnabled ? "storey " : "",
            bimFilterState_.materialFilterEnabled ? "material " : "",
            bimFilterState_.disciplineFilterEnabled ? "discipline " : "",
            bimFilterState_.phaseFilterEnabled ? "phase " : "",
            bimFilterState_.fireRatingFilterEnabled ? "fire rating " : "",
            bimFilterState_.loadBearingFilterEnabled ? "load-bearing " : "",
            bimFilterState_.statusFilterEnabled ? "status " : "",
            bimFilterState_.disciplinePreset !=
                    container::renderer::BimDisciplinePreset::None
                ? "preset "
                : "",
            bimPhaseTimelineUiState_.enabled ? "timeline " : "",
            (bimFilterState_.isolateSelection || bimFilterState_.hideSelection)
                ? "selection"
                : "");

        if (bimInspection.hasSelection) {
          if (ImGui::SmallButton("Use selected type") &&
              !bimInspection.type.empty()) {
            bimFilterState_.typeFilterEnabled = true;
            bimFilterState_.type = bimInspection.type;
          }
          ImGui::SameLine();
          const std::string selectedStorey = !bimInspection.storeyName.empty()
                                                 ? bimInspection.storeyName
                                                 : bimInspection.storeyId;
          if (ImGui::SmallButton("Use selected storey") &&
              !selectedStorey.empty()) {
            bimFilterState_.storeyFilterEnabled = true;
            bimFilterState_.storey = selectedStorey;
          }
          ImGui::SameLine();
          const std::string selectedMaterial =
              !bimInspection.materialName.empty()
                  ? bimInspection.materialName
                  : bimInspection.materialCategory;
          if (ImGui::SmallButton("Use selected material") &&
              !selectedMaterial.empty()) {
            bimFilterState_.materialFilterEnabled = true;
            bimFilterState_.material = selectedMaterial;
          }
        }

        if (bimQuickFilterSearch_.empty()) {
          ImGui::TextDisabled("Type to search exposed BIM type, storey, "
                              "material, phase, and status values");
        } else if (ImGui::BeginTable("BimQuickFilterMatches", 3,
                                     ImGuiTableFlags_Borders |
                                         ImGuiTableFlags_RowBg |
                                         ImGuiTableFlags_SizingStretchProp,
                                     ImVec2(0.0f, 220.0f))) {
          ImGui::TableSetupColumn("Category");
          ImGui::TableSetupColumn("Value");
          ImGui::TableSetupColumn("Action");
          ImGui::TableHeadersRow();
          size_t visibleMatches = 0u;
          size_t totalMatches = 0u;
          auto drawFilterMatches =
              [&](const char *category, std::span<const std::string> values,
                  bool &filterEnabled, std::string &filterValue) {
                for (const std::string &value : values) {
                  if (!ContainsCaseInsensitive(value, bimQuickFilterSearch_)) {
                    continue;
                  }
                  ++totalMatches;
                  if (visibleMatches >= 96u) {
                    continue;
                  }
                  ImGui::PushID(category);
                  ImGui::PushID(static_cast<int>(visibleMatches));
                  ImGui::TableNextRow();
                  ImGui::TableNextColumn();
                  ImGui::TextUnformatted(category);
                  ImGui::TableNextColumn();
                  ImGui::TextWrapped("%s", value.c_str());
                  ImGui::TableNextColumn();
                  if (ImGui::SmallButton("Use")) {
                    filterEnabled = true;
                    filterValue = value;
                  }
                  ImGui::PopID();
                  ImGui::PopID();
                  ++visibleMatches;
                }
              };
          drawFilterMatches("Type", bimInspection.elementTypes,
                            bimFilterState_.typeFilterEnabled,
                            bimFilterState_.type);
          drawFilterMatches("Storey", bimInspection.elementStoreys,
                            bimFilterState_.storeyFilterEnabled,
                            bimFilterState_.storey);
          drawFilterMatches("Material", bimInspection.elementMaterials,
                            bimFilterState_.materialFilterEnabled,
                            bimFilterState_.material);
          drawFilterMatches("Discipline", bimInspection.elementDisciplines,
                            bimFilterState_.disciplineFilterEnabled,
                            bimFilterState_.discipline);
          drawFilterMatches("Phase", bimInspection.elementPhases,
                            bimFilterState_.phaseFilterEnabled,
                            bimFilterState_.phase);
          drawFilterMatches("Fire rating", bimInspection.elementFireRatings,
                            bimFilterState_.fireRatingFilterEnabled,
                            bimFilterState_.fireRating);
          drawFilterMatches("Load-bearing",
                            bimInspection.elementLoadBearingValues,
                            bimFilterState_.loadBearingFilterEnabled,
                            bimFilterState_.loadBearing);
          drawFilterMatches("Status", bimInspection.elementStatuses,
                            bimFilterState_.statusFilterEnabled,
                            bimFilterState_.status);
          ImGui::EndTable();
          if (totalMatches == 0u) {
            ImGui::TextDisabled("No filter values match the search");
          } else if (visibleMatches < totalMatches) {
            ImGui::TextDisabled("%zu more matches",
                                totalMatches - visibleMatches);
          }
        }
        ImGui::TreePop();
      }

      if (ImGui::TreeNode("Selection Sets")) {
        ImGui::InputText("Selection set name", &bimSelectionSetNameInput_);
        auto makeSelectionSetLabel = [&]() {
          if (!bimSelectionSetNameInput_.empty()) {
            return bimSelectionSetNameInput_;
          }
          return "Selection Set " + std::to_string(nextBimSelectionSetId_);
        };
        auto makeSelectionSetMember = [&]() {
          BimSelectionSetMemberState member{};
          member.label = BimMeasurementSelectionLabel(bimInspection);
          member.type = bimInspection.type;
          member.storey = !bimInspection.storeyName.empty()
                              ? bimInspection.storeyName
                              : bimInspection.storeyId;
          member.material = !bimInspection.materialName.empty()
                                ? bimInspection.materialName
                                : bimInspection.materialCategory;
          member.snapshot = currentViewpoint;
          member.snapshot.bimModelPath = bimInspection.modelPath;
          member.snapshot.selectedBimObjectIndex =
              bimInspection.selectedObjectIndex;
          member.snapshot.selectedBimGuid = bimInspection.guid;
          member.snapshot.selectedBimType = bimInspection.type;
          member.snapshot.selectedBimSourceId = bimInspection.sourceId;
          member.snapshot.bimFilter = bimFilterState_;
          member.snapshot.phaseTimeline = bimPhaseTimelineUiState_;
          return member;
        };
        auto addSelectionMemberToSet = [&](BimSelectionSetState &set,
                                           BimSelectionSetMemberState member) {
          const auto duplicate = std::ranges::find_if(
              set.members, [&](const BimSelectionSetMemberState &existing) {
                const auto &lhs = existing.snapshot;
                const auto &rhs = member.snapshot;
                return lhs.selectedBimObjectIndex ==
                           rhs.selectedBimObjectIndex ||
                       (!lhs.selectedBimGuid.empty() &&
                        lhs.selectedBimGuid == rhs.selectedBimGuid) ||
                       (!lhs.selectedBimSourceId.empty() &&
                        lhs.selectedBimSourceId == rhs.selectedBimSourceId);
              });
          if (duplicate != set.members.end()) {
            statusMessage_ = "Selection set already contains " + member.label;
            return;
          }
          set.members.push_back(std::move(member));
          statusMessage_ = "Added BIM selection to " + set.label;
        };

        if (ImGui::SmallButton("Create selection set")) {
          BimSelectionSetState set{};
          set.id = nextBimSelectionSetId_++;
          set.label = makeSelectionSetLabel();
          set.modelPath = bimInspection.modelPath;
          bimSelectionSets_.push_back(std::move(set));
          selectedBimSelectionSetIndex_ =
              static_cast<int>(bimSelectionSets_.size()) - 1;
        }
        if (bimInspection.hasSelection) {
          ImGui::SameLine();
          if (ImGui::SmallButton("Create set from selection")) {
            BimSelectionSetState set{};
            set.id = nextBimSelectionSetId_++;
            set.label = makeSelectionSetLabel();
            set.modelPath = bimInspection.modelPath;
            set.members.push_back(makeSelectionSetMember());
            bimSelectionSets_.push_back(std::move(set));
            selectedBimSelectionSetIndex_ =
                static_cast<int>(bimSelectionSets_.size()) - 1;
            statusMessage_ = "Created BIM selection set";
          }
        }

        if (bimSelectionSets_.empty()) {
          selectedBimSelectionSetIndex_ = -1;
          ImGui::TextDisabled("No BIM selection sets");
        } else {
          selectedBimSelectionSetIndex_ =
              std::clamp(selectedBimSelectionSetIndex_, 0,
                         static_cast<int>(bimSelectionSets_.size()) - 1);
          const std::string preview =
              bimSelectionSets_[static_cast<size_t>(
                                    selectedBimSelectionSetIndex_)]
                  .label;
          if (ImGui::BeginCombo("Active selection set", preview.c_str())) {
            for (size_t i = 0; i < bimSelectionSets_.size(); ++i) {
              const bool selected =
                  selectedBimSelectionSetIndex_ == static_cast<int>(i);
              if (ImGui::Selectable(bimSelectionSets_[i].label.c_str(),
                                    selected)) {
                selectedBimSelectionSetIndex_ = static_cast<int>(i);
              }
              if (selected) {
                ImGui::SetItemDefaultFocus();
              }
            }
            ImGui::EndCombo();
          }

          BimSelectionSetState &set = bimSelectionSets_[static_cast<size_t>(
              selectedBimSelectionSetIndex_)];
          ImGui::Text("Members: %zu", set.members.size());
          if (bimInspection.hasSelection) {
            if (ImGui::SmallButton("Add selection to set")) {
              addSelectionMemberToSet(set, makeSelectionSetMember());
            }
          } else {
            ImGui::BeginDisabled();
            ImGui::SmallButton("Add selection to set");
            ImGui::EndDisabled();
          }
          ImGui::SameLine();
          if (ImGui::SmallButton("Clear set")) {
            set.members.clear();
            statusMessage_ = "Cleared BIM selection set";
          }
          ImGui::SameLine();
          if (ImGui::SmallButton("Delete set")) {
            bimSelectionSets_.erase(bimSelectionSets_.begin() +
                                    selectedBimSelectionSetIndex_);
            if (bimSelectionSets_.empty()) {
              selectedBimSelectionSetIndex_ = -1;
            } else {
              selectedBimSelectionSetIndex_ =
                  std::min<int>(selectedBimSelectionSetIndex_,
                                static_cast<int>(bimSelectionSets_.size()) - 1);
            }
          }

          int removeMemberIndex = -1;
          if (!set.members.empty() &&
              ImGui::BeginTable("BimSelectionSetMembers", 5,
                                ImGuiTableFlags_Borders |
                                    ImGuiTableFlags_RowBg |
                                    ImGuiTableFlags_SizingStretchProp,
                                ImVec2(0.0f, 180.0f))) {
            ImGui::TableSetupColumn("Element");
            ImGui::TableSetupColumn("Type");
            ImGui::TableSetupColumn("Storey");
            ImGui::TableSetupColumn("Material");
            ImGui::TableSetupColumn("Action");
            ImGui::TableHeadersRow();
            for (size_t i = 0; i < set.members.size(); ++i) {
              const BimSelectionSetMemberState &member = set.members[i];
              ImGui::PushID(static_cast<int>(i));
              ImGui::TableNextRow();
              ImGui::TableNextColumn();
              ImGui::TextWrapped("%s", member.label.c_str());
              ImGui::TableNextColumn();
              ImGui::TextWrapped("%s", member.type.c_str());
              ImGui::TableNextColumn();
              ImGui::TextWrapped("%s", member.storey.c_str());
              ImGui::TableNextColumn();
              ImGui::TextWrapped("%s", member.material.c_str());
              ImGui::TableNextColumn();
              if (ImGui::SmallButton("Restore")) {
                const bool restored = restoreViewpoint
                                          ? restoreViewpoint(member.snapshot)
                                          : false;
                statusMessage_ =
                    restored ? "Restored BIM selection set member"
                             : "Failed to restore BIM selection set member";
              }
              ImGui::SameLine();
              if (ImGui::SmallButton("Remove")) {
                removeMemberIndex = static_cast<int>(i);
              }
              ImGui::PopID();
            }
            ImGui::EndTable();
          }
          if (removeMemberIndex >= 0) {
            set.members.erase(set.members.begin() + removeMemberIndex);
          }
        }
        ImGui::TextDisabled("Selection sets are stored in UI memory; "
                            "multi-select rendering is not connected");
        ImGui::TreePop();
      }

      if (ImGui::TreeNode("Property Browser")) {
        if (bimInspection.hasSelection) {
          auto drawPropertyRow = [](const char *label,
                                    const std::string &value) {
            if (value.empty()) {
              return;
            }
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", label);
            ImGui::TableNextColumn();
            ImGui::TextWrapped("%s", value.c_str());
          };
          auto drawPropertyIndexRow = [](const char *label, uint32_t value) {
            if (value == std::numeric_limits<uint32_t>::max()) {
              return;
            }
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", label);
            ImGui::TableNextColumn();
            ImGui::Text("%u", value);
          };
          if (ImGui::BeginTable("BimPropertyBrowser", 2,
                                ImGuiTableFlags_BordersInnerV |
                                    ImGuiTableFlags_RowBg |
                                    ImGuiTableFlags_SizingStretchProp)) {
            drawPropertyIndexRow("Object index",
                                 bimInspection.selectedObjectIndex);
            drawPropertyRow("Name", bimInspection.displayName);
            drawPropertyRow("IFC class / type", bimInspection.type);
            drawPropertyRow("Object type", bimInspection.objectType);
            drawPropertyRow("GUID", bimInspection.guid);
            drawPropertyRow("Source ID", bimInspection.sourceId);
            drawPropertyRow("Storey", bimInspection.storeyName);
            drawPropertyRow("Storey ID", bimInspection.storeyId);
            drawPropertyRow("Material", bimInspection.materialName);
            drawPropertyRow("Material category",
                            bimInspection.materialCategory);
            drawPropertyRow("Discipline", bimInspection.discipline);
            drawPropertyRow("Phase", bimInspection.phase);
            drawPropertyRow("Fire rating", bimInspection.fireRating);
            drawPropertyRow("Load-bearing", bimInspection.loadBearing);
            drawPropertyRow("Status", bimInspection.status);
            drawPropertyRow("Geometry", bimInspection.geometryKind);
            drawPropertyIndexRow("Source element",
                                 bimInspection.sourceElementIndex);
            drawPropertyIndexRow("Mesh", bimInspection.meshId);
            drawPropertyIndexRow("Source material",
                                 bimInspection.sourceMaterialIndex);
            drawPropertyIndexRow("Render material",
                                 bimInspection.materialIndex);
            drawPropertyIndexRow("Semantic type", bimInspection.semanticTypeId);
            ImGui::EndTable();
          }
          if (ImGui::TreeNode("Extended properties")) {
            if (bimInspection.properties.empty()) {
              ImGui::TextDisabled(
                  "No generic element properties are exposed for this element");
            } else {
              ImGui::InputText("Property search", &bimPropertySearch_);
              ImGui::SameLine();
              if (ImGui::SmallButton("Clear property search")) {
                bimPropertySearch_.clear();
              }
              if (ImGui::BeginTable("BimExtendedProperties", 4,
                                    ImGuiTableFlags_Borders |
                                        ImGuiTableFlags_RowBg |
                                        ImGuiTableFlags_SizingStretchProp,
                                    ImVec2(0.0f, 220.0f))) {
                ImGui::TableSetupColumn("Set");
                ImGui::TableSetupColumn("Name");
                ImGui::TableSetupColumn("Value");
                ImGui::TableSetupColumn("Category");
                ImGui::TableHeadersRow();
                size_t visiblePropertyCount = 0u;
                size_t matchedPropertyCount = 0u;
                for (size_t i = 0; i < bimInspection.properties.size(); ++i) {
                  const container::renderer::BimElementProperty &property =
                      bimInspection.properties[i];
                  if (!PropertyMatchesSearch(property, bimPropertySearch_)) {
                    continue;
                  }
                  ++matchedPropertyCount;
                  if (visiblePropertyCount >= 128u) {
                    continue;
                  }
                  ++visiblePropertyCount;
                  ImGui::TableNextRow();
                  ImGui::TableNextColumn();
                  ImGui::TextWrapped("%s", property.set.c_str());
                  ImGui::TableNextColumn();
                  ImGui::TextWrapped("%s", property.name.c_str());
                  ImGui::TableNextColumn();
                  ImGui::TextWrapped("%s", property.value.c_str());
                  ImGui::TableNextColumn();
                  ImGui::TextWrapped("%s", property.category.c_str());
                }
                ImGui::EndTable();
                if (matchedPropertyCount == 0u) {
                  ImGui::TextDisabled("No properties match the search");
                } else if (visiblePropertyCount < matchedPropertyCount) {
                  ImGui::TextDisabled("%zu more matching properties",
                                      matchedPropertyCount -
                                          visiblePropertyCount);
                }
              }
            }
            ImGui::TreePop();
          }
        } else {
          ImGui::TextDisabled("No selected BIM element");
          ImGui::TextDisabled(
              "Select an element to inspect generic properties");
        }
        ImGui::TreePop();
      }

      if (ImGui::TreeNode("IFC Relationships")) {
        const container::renderer::BimRelationshipGraph *relationshipGraph =
            bimInspection.relationshipGraph;
        if (relationshipGraph != nullptr) {
          ImGui::Text("Graph: %zu nodes, %zu edges",
                      relationshipGraph->nodes().size(),
                      relationshipGraph->edges().size());
          ImGui::InputText("Relationship search", &bimRelationshipSearch_);
          ImGui::SameLine();
          if (ImGui::SmallButton("Clear relationship search")) {
            bimRelationshipSearch_.clear();
          }
          if (!bimRelationshipSearch_.empty()) {
            const auto hits = relationshipGraph->search(bimRelationshipSearch_);
            if (hits.empty()) {
              ImGui::TextDisabled("No graph matches");
            } else if (ImGui::BeginTable(
                           "BimRelationshipSearchResults", 3,
                           ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                               ImGuiTableFlags_SizingStretchProp,
                           ImVec2(0.0f, 140.0f))) {
              ImGui::TableSetupColumn("Object");
              ImGui::TableSetupColumn("Reason");
              ImGui::TableSetupColumn("Match");
              ImGui::TableHeadersRow();
              const size_t visibleHitCount = std::min<size_t>(hits.size(), 64u);
              for (size_t i = 0; i < visibleHitCount; ++i) {
                const auto &hit = hits[i];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Text("%u", hit.objectIndex);
                ImGui::TableNextColumn();
                ImGui::TextWrapped("%s", hit.reason.c_str());
                ImGui::TableNextColumn();
                ImGui::TextWrapped("%s", hit.matchedText.c_str());
              }
              ImGui::EndTable();
              if (visibleHitCount < hits.size()) {
                ImGui::TextDisabled("%zu more matches",
                                    hits.size() - visibleHitCount);
              }
            }
          }
        }
        if (bimInspection.hasSelection) {
          auto drawRelationshipRow =
              [&](const char *relationship, const std::string &value,
                  const char *actionLabel,
                  const std::function<void()> &applyAction) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%s", relationship);
                ImGui::TableNextColumn();
                if (value.empty()) {
                  ImGui::TextDisabled("not assigned");
                } else {
                  ImGui::TextWrapped("%s", value.c_str());
                }
                ImGui::TableNextColumn();
                if (value.empty()) {
                  ImGui::BeginDisabled();
                }
                if (ImGui::SmallButton(actionLabel)) {
                  applyAction();
                }
                if (value.empty()) {
                  ImGui::EndDisabled();
                }
              };
          const std::string selectedStorey = !bimInspection.storeyName.empty()
                                                 ? bimInspection.storeyName
                                                 : bimInspection.storeyId;
          const std::string selectedMaterial =
              !bimInspection.materialName.empty()
                  ? bimInspection.materialName
                  : bimInspection.materialCategory;
          const std::string selectedType = !bimInspection.objectType.empty()
                                               ? bimInspection.objectType
                                               : bimInspection.type;
          if (relationshipGraph != nullptr) {
            const auto selectedEdges =
                relationshipGraph->edgesForObject(
                    bimInspection.selectedObjectIndex);
            if (!selectedEdges.empty() &&
                ImGui::BeginTable("BimSelectedRelationshipEdges", 4,
                                  ImGuiTableFlags_Borders |
                                      ImGuiTableFlags_RowBg |
                                      ImGuiTableFlags_SizingStretchProp,
                                  ImVec2(0.0f, 150.0f))) {
              ImGui::TableSetupColumn("Relationship");
              ImGui::TableSetupColumn("Direction");
              ImGui::TableSetupColumn("Related node");
              ImGui::TableSetupColumn("Label");
              ImGui::TableHeadersRow();
              const auto nodes = relationshipGraph->nodes();
              const size_t visibleEdgeCount =
                  std::min<size_t>(selectedEdges.size(), 64u);
              for (size_t i = 0; i < visibleEdgeCount; ++i) {
                const auto &edge = selectedEdges[i];
                if (edge.from >= nodes.size() || edge.to >= nodes.size()) {
                  continue;
                }
                const bool selectedIsFrom =
                    nodes[edge.from].objectIndex ==
                    bimInspection.selectedObjectIndex;
                const auto &relatedNode =
                    selectedIsFrom ? nodes[edge.to] : nodes[edge.from];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextWrapped(
                    "%s",
                    std::string(container::renderer::bimRelationshipKindLabel(
                                    edge.kind))
                        .c_str());
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%s", selectedIsFrom ? "out" : "in");
                ImGui::TableNextColumn();
                const std::string relatedLabel =
                    RelationshipNodeLabel(relatedNode);
                ImGui::TextWrapped("%s", relatedLabel.c_str());
                ImGui::TableNextColumn();
                ImGui::TextWrapped("%s", edge.label.c_str());
              }
              ImGui::EndTable();
              if (visibleEdgeCount < selectedEdges.size()) {
                ImGui::TextDisabled("%zu more relationships",
                                    selectedEdges.size() - visibleEdgeCount);
              }
            }
          }
          if (ImGui::BeginTable("BimIfcRelationshipBrowser", 3,
                                ImGuiTableFlags_Borders |
                                    ImGuiTableFlags_RowBg |
                                    ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Relationship");
            ImGui::TableSetupColumn("Current element");
            ImGui::TableSetupColumn("Action");
            ImGui::TableHeadersRow();
            drawRelationshipRow("Spatial containment", selectedStorey,
                                "Filter related storey", [&]() {
                                  bimFilterState_.storeyFilterEnabled = true;
                                  bimFilterState_.storey = selectedStorey;
                                  statusMessage_ =
                                      "Filtered BIM relationship storey: " +
                                      selectedStorey;
                                });
            drawRelationshipRow("Material assignment", selectedMaterial,
                                "Filter related material", [&]() {
                                  bimFilterState_.materialFilterEnabled = true;
                                  bimFilterState_.material = selectedMaterial;
                                  statusMessage_ =
                                      "Filtered BIM relationship material: " +
                                      selectedMaterial;
                                });
            drawRelationshipRow("Type/classification", selectedType,
                                "Filter related type", [&]() {
                                  bimFilterState_.typeFilterEnabled = true;
                                  bimFilterState_.type = selectedType;
                                  statusMessage_ =
                                      "Filtered BIM relationship type: " +
                                      selectedType;
                                });
            drawRelationshipRow(
                "Identity",
                !bimInspection.guid.empty() ? bimInspection.guid
                                            : bimInspection.sourceId,
                "Copy via selection set", [&]() {
                  BimSelectionSetState set{};
                  set.id = nextBimSelectionSetId_++;
                  set.label = "IFC relationship selection";
                  set.modelPath = bimInspection.modelPath;
                  BimSelectionSetMemberState member{};
                  member.label = BimMeasurementSelectionLabel(bimInspection);
                  member.type = bimInspection.type;
                  member.storey = selectedStorey;
                  member.material = selectedMaterial;
                  member.snapshot = currentViewpoint;
                  member.snapshot.bimModelPath = bimInspection.modelPath;
                  member.snapshot.selectedBimObjectIndex =
                      bimInspection.selectedObjectIndex;
                  member.snapshot.selectedBimGuid = bimInspection.guid;
                  member.snapshot.selectedBimType = bimInspection.type;
                  member.snapshot.selectedBimSourceId = bimInspection.sourceId;
                  member.snapshot.bimFilter = bimFilterState_;
                  member.snapshot.phaseTimeline = bimPhaseTimelineUiState_;
                  set.members.push_back(std::move(member));
                  bimSelectionSets_.push_back(std::move(set));
                  selectedBimSelectionSetIndex_ =
                      static_cast<int>(bimSelectionSets_.size()) - 1;
                  statusMessage_ = "Created IFC relationship selection set";
                });
            ImGui::EndTable();
          }
          const auto graphPropertySets =
              relationshipGraph != nullptr
                  ? relationshipGraph->propertySetsForObject(
                        bimInspection.selectedObjectIndex)
                  : std::span<const container::renderer::BimPropertySetGroup>{};
          if (!graphPropertySets.empty()) {
            const size_t visiblePropertySetCount =
                std::min<size_t>(graphPropertySets.size(), 32u);
            for (size_t setIndex = 0; setIndex < visiblePropertySetCount;
                 ++setIndex) {
              const auto &group = graphPropertySets[setIndex];
              ImGui::PushID(static_cast<int>(setIndex));
              const std::string label =
                  group.set + " (" + std::to_string(group.properties.size()) +
                  ")";
              if (ImGui::TreeNode(label.c_str())) {
                if (ImGui::BeginTable("BimIfcPropertySetProperties", 3,
                                      ImGuiTableFlags_Borders |
                                          ImGuiTableFlags_RowBg |
                                          ImGuiTableFlags_SizingStretchProp)) {
                  ImGui::TableSetupColumn("Name");
                  ImGui::TableSetupColumn("Value");
                  ImGui::TableSetupColumn("Category");
                  ImGui::TableHeadersRow();
                  for (const auto &property : group.properties) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextWrapped("%s", property.name.c_str());
                    ImGui::TableNextColumn();
                    ImGui::TextWrapped("%s", property.value.c_str());
                    ImGui::TableNextColumn();
                    ImGui::TextWrapped("%s", property.category.c_str());
                  }
                  ImGui::EndTable();
                }
                ImGui::TreePop();
              }
              ImGui::PopID();
            }
            if (visiblePropertySetCount < graphPropertySets.size()) {
              ImGui::TextDisabled("%zu more property sets",
                                  graphPropertySets.size() -
                                      visiblePropertySetCount);
            }
          } else if (bimInspection.properties.empty()) {
            ImGui::TextDisabled(
                "No property-set relationships are exposed for this element");
          } else if (ImGui::BeginTable("BimIfcPropertySetSummary", 3,
                                       ImGuiTableFlags_Borders |
                                           ImGuiTableFlags_RowBg |
                                           ImGuiTableFlags_SizingStretchProp,
                                       ImVec2(0.0f, 160.0f))) {
            ImGui::TableSetupColumn("Property set");
            ImGui::TableSetupColumn("Category");
            ImGui::TableSetupColumn("Properties");
            ImGui::TableHeadersRow();
            size_t visiblePropertySetCount = 0u;
            std::vector<std::string> visiblePropertySetKeys;
            for (const container::renderer::BimElementProperty &property :
                 bimInspection.properties) {
              const std::string setName =
                  !property.set.empty() ? property.set : "(unassigned set)";
              const std::string categoryName = !property.category.empty()
                                                   ? property.category
                                                   : "(unassigned category)";
              const std::string key = setName + "\n" + categoryName;
              if (std::ranges::contains(visiblePropertySetKeys, key)) {
                continue;
              }
              size_t propertyCount = 0u;
              for (const container::renderer::BimElementProperty &candidate :
                   bimInspection.properties) {
                const std::string candidateSet =
                    !candidate.set.empty() ? candidate.set : "(unassigned set)";
                const std::string candidateCategory =
                    !candidate.category.empty() ? candidate.category
                                                : "(unassigned category)";
                if (candidateSet == setName &&
                    candidateCategory == categoryName) {
                  ++propertyCount;
                }
              }
              visiblePropertySetKeys.push_back(key);
              if (visiblePropertySetCount >= 32u) {
                continue;
              }
              ++visiblePropertySetCount;
              ImGui::TableNextRow();
              ImGui::TableNextColumn();
              ImGui::TextWrapped("%s", setName.c_str());
              ImGui::TableNextColumn();
              ImGui::TextWrapped("%s", categoryName.c_str());
              ImGui::TableNextColumn();
              ImGui::Text("%zu", propertyCount);
            }
            ImGui::EndTable();
            if (visiblePropertySetCount < visiblePropertySetKeys.size()) {
              ImGui::TextDisabled("%zu more property sets",
                                  visiblePropertySetKeys.size() -
                                  visiblePropertySetCount);
            }
          }
        } else {
          ImGui::TextDisabled(
              "Select an element to browse inferred IFC relationships");
        }
        ImGui::TreePop();
      }

      if (ImGui::TreeNode("Spatial Browser")) {
        if (bimInspection.relationshipGraph != nullptr &&
            !bimInspection.relationshipGraph->edges().empty() &&
            ImGui::TreeNode("Graph spatial edges")) {
          const auto nodes = bimInspection.relationshipGraph->nodes();
          const auto edges = bimInspection.relationshipGraph->edges();
          if (ImGui::BeginTable("BimGraphSpatialEdges", 3,
                                ImGuiTableFlags_Borders |
                                    ImGuiTableFlags_RowBg |
                                    ImGuiTableFlags_SizingStretchProp,
                                ImVec2(0.0f, 180.0f))) {
            ImGui::TableSetupColumn("Parent");
            ImGui::TableSetupColumn("Child");
            ImGui::TableSetupColumn("Label");
            ImGui::TableHeadersRow();
            size_t visibleSpatialEdgeCount = 0u;
            size_t spatialEdgeCount = 0u;
            for (const auto &edge : edges) {
              if (edge.kind !=
                      container::renderer::BimRelationshipKind::SpatialParent ||
                  edge.from >= nodes.size() || edge.to >= nodes.size()) {
                continue;
              }
              ++spatialEdgeCount;
              if (visibleSpatialEdgeCount >= 128u) {
                continue;
              }
              ++visibleSpatialEdgeCount;
              const std::string parent = RelationshipNodeLabel(nodes[edge.from]);
              const std::string child = RelationshipNodeLabel(nodes[edge.to]);
              ImGui::TableNextRow();
              ImGui::TableNextColumn();
              ImGui::TextWrapped("%s", parent.c_str());
              ImGui::TableNextColumn();
              ImGui::TextWrapped("%s", child.c_str());
              ImGui::TableNextColumn();
              ImGui::TextWrapped("%s", edge.label.c_str());
            }
            ImGui::EndTable();
            if (visibleSpatialEdgeCount < spatialEdgeCount) {
              ImGui::TextDisabled("%zu more spatial edges",
                                  spatialEdgeCount - visibleSpatialEdgeCount);
            }
          }
          ImGui::TreePop();
        }
        if (bimInspection.elementStoreyRanges.empty() &&
            bimInspection.elementStoreys.empty()) {
          ImGui::TextDisabled("No spatial hierarchy exposed by renderer");
        } else if (!bimInspection.elementStoreyRanges.empty()) {
          const int storeyRangeCount = static_cast<int>(std::min<size_t>(
              bimInspection.elementStoreyRanges.size(),
              static_cast<size_t>(std::numeric_limits<int>::max())));
          if (selectedBimStoreyRangeIndex_ >= storeyRangeCount) {
            selectedBimStoreyRangeIndex_ = storeyRangeCount - 1;
          }
          if (selectedBimStoreyRangeIndex_ < 0 &&
              bimFilterState_.storeyFilterEnabled &&
              !bimFilterState_.storey.empty()) {
            for (int i = 0; i < storeyRangeCount; ++i) {
              if (bimInspection.elementStoreyRanges[static_cast<size_t>(i)]
                      .label == bimFilterState_.storey) {
                selectedBimStoreyRangeIndex_ = i;
                break;
              }
            }
          }
          auto selectRelativePlanStorey = [&](int delta) {
            if (storeyRangeCount <= 0) {
              return;
            }
            if (selectedBimStoreyRangeIndex_ < 0) {
              selectedBimStoreyRangeIndex_ =
                  delta < 0 ? storeyRangeCount - 1 : 0;
              return;
            }
            selectedBimStoreyRangeIndex_ = std::clamp(
                selectedBimStoreyRangeIndex_ + delta, 0, storeyRangeCount - 1);
          };
          const char *planStoreyPreview =
              selectedBimStoreyRangeIndex_ >= 0
                  ? bimInspection
                        .elementStoreyRanges[static_cast<size_t>(
                            selectedBimStoreyRangeIndex_)]
                        .label.c_str()
                  : "Select storey";
          if (ImGui::BeginCombo("Plan storey", planStoreyPreview)) {
            if (ImGui::Selectable("Select storey",
                                  selectedBimStoreyRangeIndex_ < 0)) {
              selectedBimStoreyRangeIndex_ = -1;
            }
            for (int i = 0; i < storeyRangeCount; ++i) {
              const auto &storeyRange =
                  bimInspection.elementStoreyRanges[static_cast<size_t>(i)];
              const bool selected = selectedBimStoreyRangeIndex_ == i;
              char label[256]{};
              std::snprintf(label, sizeof(label), "%s (%.2f..%.2f)",
                            storeyRange.label.c_str(), storeyRange.minElevation,
                            storeyRange.maxElevation);
              if (ImGui::Selectable(label, selected)) {
                selectedBimStoreyRangeIndex_ = i;
              }
              if (selected) {
                ImGui::SetItemDefaultFocus();
              }
            }
            ImGui::EndCombo();
          }
          if (ImGui::SmallButton("Previous storey")) {
            selectRelativePlanStorey(-1);
          }
          ImGui::SameLine();
          if (ImGui::SmallButton("Next storey")) {
            selectRelativePlanStorey(1);
          }

          const bool hasPlanStorey =
              selectedBimStoreyRangeIndex_ >= 0 &&
              selectedBimStoreyRangeIndex_ < storeyRangeCount;
          if (hasPlanStorey) {
            const auto &storeyRange =
                bimInspection.elementStoreyRanges[static_cast<size_t>(
                    selectedBimStoreyRangeIndex_)];
            ImGui::Text("Plan navigation storey: %s  %.2f..%.2f",
                        storeyRange.label.c_str(), storeyRange.minElevation,
                        storeyRange.maxElevation);
          }
          if (!hasPlanStorey) {
            ImGui::BeginDisabled();
          }
          if (ImGui::SmallButton("Show storey only")) {
            const auto &storeyRange =
                bimInspection.elementStoreyRanges[static_cast<size_t>(
                    selectedBimStoreyRangeIndex_)];
            applyStoreyFilter(storeyRange);
            statusMessage_ =
                "Applied storey visibility preset: " + storeyRange.label;
          }
          ImGui::SameLine();
          if (ImGui::SmallButton("Plan slice preset")) {
            const auto &storeyRange =
                bimInspection.elementStoreyRanges[static_cast<size_t>(
                    selectedBimStoreyRangeIndex_)];
            applyStoreyPlanSlice(storeyRange, true);
            statusMessage_ = "Applied storey plan preset: " + storeyRange.label;
          }
          if (!hasPlanStorey) {
            ImGui::EndDisabled();
          }
          ImGui::SameLine();
          if (ImGui::SmallButton("Clear storey preset")) {
            clearStoreyPlanPreset();
            statusMessage_ = "Cleared storey visibility preset";
          }

          if (ImGui::BeginTable("BimSpatialStoreys", 4,
                                ImGuiTableFlags_Borders |
                                    ImGuiTableFlags_RowBg |
                                    ImGuiTableFlags_SizingStretchProp,
                                ImVec2(0.0f, 220.0f))) {
            ImGui::TableSetupColumn("Storey");
            ImGui::TableSetupColumn("Elevation");
            ImGui::TableSetupColumn("Objects");
            ImGui::TableSetupColumn("Preset");
            ImGui::TableHeadersRow();
            for (int i = 0; i < storeyRangeCount; ++i) {
              const auto &storeyRange =
                  bimInspection.elementStoreyRanges[static_cast<size_t>(i)];
              ImGui::PushID(i);
              ImGui::TableNextRow();
              ImGui::TableNextColumn();
              ImGui::TextWrapped("%s", storeyRange.label.c_str());
              ImGui::TableNextColumn();
              ImGui::Text("%.2f..%.2f", storeyRange.minElevation,
                          storeyRange.maxElevation);
              ImGui::TableNextColumn();
              ImGui::Text("%zu", storeyRange.objectCount);
              ImGui::TableNextColumn();
              if (ImGui::SmallButton("Filter")) {
                selectedBimStoreyRangeIndex_ = i;
                applyStoreyFilter(storeyRange);
              }
              ImGui::SameLine();
              if (ImGui::SmallButton("Plan")) {
                selectedBimStoreyRangeIndex_ = i;
                applyStoreyPlanSlice(storeyRange, true);
              }
              if (floorSliceMatchesStorey(storeyRange)) {
                ImGui::SameLine();
                ImGui::TextDisabled("active");
              }
              ImGui::PopID();
            }
            ImGui::EndTable();
          }
        } else {
          const size_t visibleStoreyCount =
              std::min<size_t>(bimInspection.elementStoreys.size(), 64u);
          for (size_t i = 0; i < visibleStoreyCount; ++i) {
            ImGui::PushID(static_cast<int>(i));
            ImGui::BulletText("%s", bimInspection.elementStoreys[i].c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("Filter")) {
              bimFilterState_.storeyFilterEnabled = true;
              bimFilterState_.storey = bimInspection.elementStoreys[i];
            }
            ImGui::PopID();
          }
          if (visibleStoreyCount < bimInspection.elementStoreys.size()) {
            ImGui::TextDisabled("%zu more storeys",
                                bimInspection.elementStoreys.size() -
                                    visibleStoreyCount);
          }
          ImGui::TextDisabled(
              "Storey elevation ranges are not exposed for plan slicing");
        }
        ImGui::TreePop();
      }

      std::string currentTypeLabel =
          bimFilterState_.typeFilterEnabled && !bimFilterState_.type.empty()
              ? bimFilterState_.type
              : "All types";
      const bool currentTypeKnown =
          !bimFilterState_.typeFilterEnabled ||
          std::ranges::contains(bimInspection.elementTypes,
                                bimFilterState_.type);
      if (!currentTypeKnown) {
        currentTypeLabel = "All types";
        bimFilterState_.typeFilterEnabled = false;
        bimFilterState_.type.clear();
      }
      if (ImGui::BeginCombo("Type filter", currentTypeLabel.c_str())) {
        const bool allSelected = !bimFilterState_.typeFilterEnabled;
        if (ImGui::Selectable("All types", allSelected)) {
          bimFilterState_.typeFilterEnabled = false;
          bimFilterState_.type.clear();
        }
        if (allSelected) {
          ImGui::SetItemDefaultFocus();
        }
        for (const std::string &type : bimInspection.elementTypes) {
          const bool selected =
              bimFilterState_.typeFilterEnabled && bimFilterState_.type == type;
          if (ImGui::Selectable(type.c_str(), selected)) {
            bimFilterState_.typeFilterEnabled = true;
            bimFilterState_.type = type;
          }
          if (selected) {
            ImGui::SetItemDefaultFocus();
          }
        }
        ImGui::EndCombo();
      }
      std::string currentStoreyLabel =
          bimFilterState_.storeyFilterEnabled && !bimFilterState_.storey.empty()
              ? bimFilterState_.storey
              : "All storeys";
      const bool currentStoreyKnown =
          !bimFilterState_.storeyFilterEnabled ||
          std::ranges::contains(bimInspection.elementStoreys,
                                bimFilterState_.storey);
      if (!currentStoreyKnown) {
        const bool floorSliceUsesStoreyFilter =
            sectionPlaneState_.enabled && sectionPlaneAxis_ == 1 &&
            sectionPlaneState_.normal.y < 0.0f &&
            bimFilterState_.storeyFilterEnabled &&
            !bimFilterState_.storey.empty();
        currentStoreyLabel = "All storeys";
        bimFilterState_.storeyFilterEnabled = false;
        bimFilterState_.storey.clear();
        if (floorSliceUsesStoreyFilter) {
          sectionPlaneState_ = {};
          sectionPlaneAxis_ = 1;
        }
      }
      if (ImGui::BeginCombo("Storey filter", currentStoreyLabel.c_str())) {
        const bool allSelected = !bimFilterState_.storeyFilterEnabled;
        if (ImGui::Selectable("All storeys", allSelected)) {
          bimFilterState_.storeyFilterEnabled = false;
          bimFilterState_.storey.clear();
        }
        if (allSelected) {
          ImGui::SetItemDefaultFocus();
        }
        for (const std::string &storey : bimInspection.elementStoreys) {
          const bool selected = bimFilterState_.storeyFilterEnabled &&
                                bimFilterState_.storey == storey;
          if (ImGui::Selectable(storey.c_str(), selected)) {
            bimFilterState_.storeyFilterEnabled = true;
            bimFilterState_.storey = storey;
          }
          if (selected) {
            ImGui::SetItemDefaultFocus();
          }
        }
        ImGui::EndCombo();
      }
      if (!bimInspection.elementStoreyRanges.empty()) {
        std::string currentFloorSliceLabel = "No floor slice";
        if (sectionPlaneState_.enabled && sectionPlaneAxis_ == 1 &&
            sectionPlaneState_.normal.y < 0.0f &&
            bimFilterState_.storeyFilterEnabled &&
            !bimFilterState_.storey.empty()) {
          currentFloorSliceLabel = bimFilterState_.storey;
        }
        if (ImGui::BeginCombo("Floor slice", currentFloorSliceLabel.c_str())) {
          const bool noSliceSelected = !sectionPlaneState_.enabled ||
                                       sectionPlaneAxis_ != 1 ||
                                       sectionPlaneState_.normal.y >= 0.0f;
          if (ImGui::Selectable("No floor slice", noSliceSelected)) {
            sectionPlaneState_.enabled = false;
          }
          if (noSliceSelected) {
            ImGui::SetItemDefaultFocus();
          }
          for (const container::renderer::BimStoreyRange &storeyRange :
               bimInspection.elementStoreyRanges) {
            char label[256]{};
            std::snprintf(label, sizeof(label), "%s (%.2f..%.2f)",
                          storeyRange.label.c_str(), storeyRange.minElevation,
                          storeyRange.maxElevation);
            const bool selected = sectionPlaneState_.enabled &&
                                  sectionPlaneAxis_ == 1 &&
                                  sectionPlaneState_.normal.y < 0.0f &&
                                  bimFilterState_.storeyFilterEnabled &&
                                  bimFilterState_.storey == storeyRange.label;
            if (ImGui::Selectable(label, selected)) {
              const float height =
                  storeyRange.maxElevation - storeyRange.minElevation;
              const float margin = std::max(0.05f, height * 0.03f);
              bimFilterState_.storeyFilterEnabled = true;
              bimFilterState_.storey = storeyRange.label;
              sectionPlaneState_.enabled = true;
              sectionPlaneAxis_ = 1;
              sectionPlaneState_.normal = {0.0f, -1.0f, 0.0f};
              sectionPlaneState_.offset = -(storeyRange.maxElevation + margin);
            }
            if (selected) {
              ImGui::SetItemDefaultFocus();
            }
          }
          ImGui::EndCombo();
        }
      }
      std::string currentMaterialLabel =
          bimFilterState_.materialFilterEnabled &&
                  !bimFilterState_.material.empty()
              ? bimFilterState_.material
              : "All materials";
      const bool currentMaterialKnown =
          !bimFilterState_.materialFilterEnabled ||
          std::ranges::contains(bimInspection.elementMaterials,
                                bimFilterState_.material);
      if (!currentMaterialKnown) {
        currentMaterialLabel = "All materials";
        bimFilterState_.materialFilterEnabled = false;
        bimFilterState_.material.clear();
      }
      if (ImGui::BeginCombo("Material filter", currentMaterialLabel.c_str())) {
        const bool allSelected = !bimFilterState_.materialFilterEnabled;
        if (ImGui::Selectable("All materials", allSelected)) {
          bimFilterState_.materialFilterEnabled = false;
          bimFilterState_.material.clear();
        }
        if (allSelected) {
          ImGui::SetItemDefaultFocus();
        }
        for (const std::string &material : bimInspection.elementMaterials) {
          const bool selected = bimFilterState_.materialFilterEnabled &&
                                bimFilterState_.material == material;
          if (ImGui::Selectable(material.c_str(), selected)) {
            bimFilterState_.materialFilterEnabled = true;
            bimFilterState_.material = material;
          }
          if (selected) {
            ImGui::SetItemDefaultFocus();
          }
        }
        ImGui::EndCombo();
      }
      if (ImGui::BeginCombo(
              "Discipline preset",
              BimDisciplinePresetLabel(bimFilterState_.disciplinePreset))) {
        for (const auto preset :
             {container::renderer::BimDisciplinePreset::None,
              container::renderer::BimDisciplinePreset::Architecture,
              container::renderer::BimDisciplinePreset::MepXray}) {
          const bool selected = bimFilterState_.disciplinePreset == preset;
          if (ImGui::Selectable(BimDisciplinePresetLabel(preset), selected)) {
            bimFilterState_.disciplinePreset = preset;
          }
          if (selected) {
            ImGui::SetItemDefaultFocus();
          }
        }
        ImGui::EndCombo();
      }
      drawStringFilterCombo("Discipline filter", "All disciplines",
                            bimInspection.elementDisciplines,
                            bimFilterState_.disciplineFilterEnabled,
                            bimFilterState_.discipline);
      drawStringFilterCombo(
          "Phase filter", "All phases", bimInspection.elementPhases,
          bimFilterState_.phaseFilterEnabled, bimFilterState_.phase);
      if (ImGui::TreeNode("Phase Timeline")) {
        ImGui::Checkbox("Enable phase timeline",
                        &bimPhaseTimelineUiState_.enabled);
        if (bimInspection.elementPhases.empty()) {
          ImGui::TextDisabled("No phase metadata");
        } else {
          const uint32_t maxPhaseIndex = static_cast<uint32_t>(
              std::min<size_t>(bimInspection.elementPhases.size() - 1u,
                               std::numeric_limits<uint32_t>::max()));
          bimPhaseTimelineUiState_.activePhaseIndex =
              std::min(bimPhaseTimelineUiState_.activePhaseIndex,
                       maxPhaseIndex);
          const std::string &activePhase =
              bimInspection.elementPhases
                  [static_cast<size_t>(
                      bimPhaseTimelineUiState_.activePhaseIndex)];
          if (ImGui::BeginCombo("Active phase", activePhase.c_str())) {
            for (size_t i = 0; i < bimInspection.elementPhases.size(); ++i) {
              const bool selected =
                  bimPhaseTimelineUiState_.activePhaseIndex ==
                  static_cast<uint32_t>(std::min<size_t>(
                      i, std::numeric_limits<uint32_t>::max()));
              if (ImGui::Selectable(bimInspection.elementPhases[i].c_str(),
                                    selected)) {
                bimPhaseTimelineUiState_.activePhaseIndex =
                    static_cast<uint32_t>(std::min<size_t>(
                        i, std::numeric_limits<uint32_t>::max()));
              }
              if (selected) {
                ImGui::SetItemDefaultFocus();
              }
            }
            ImGui::EndCombo();
          }
        }
        ImGui::Checkbox("Show existing",
                        &bimPhaseTimelineUiState_.showExisting);
        ImGui::Checkbox("Show new", &bimPhaseTimelineUiState_.showNew);
        ImGui::Checkbox("Show demolished",
                        &bimPhaseTimelineUiState_.showDemolished);
        ImGui::Checkbox("Ghost future",
                        &bimPhaseTimelineUiState_.ghostFuture);
        ImGui::TreePop();
      }
      drawStringFilterCombo("Fire rating filter", "All fire ratings",
                            bimInspection.elementFireRatings,
                            bimFilterState_.fireRatingFilterEnabled,
                            bimFilterState_.fireRating);
      drawStringFilterCombo("Load-bearing filter", "All load-bearing values",
                            bimInspection.elementLoadBearingValues,
                            bimFilterState_.loadBearingFilterEnabled,
                            bimFilterState_.loadBearing);
      drawStringFilterCombo(
          "Status filter", "All statuses", bimInspection.elementStatuses,
          bimFilterState_.statusFilterEnabled, bimFilterState_.status);
      if (!bimInspection.hasSelection) {
        ImGui::BeginDisabled();
      }
      if (ImGui::Checkbox("Isolate selected",
                          &bimFilterState_.isolateSelection) &&
          bimFilterState_.isolateSelection) {
        bimFilterState_.hideSelection = false;
      }
      if (ImGui::Checkbox("Hide selected", &bimFilterState_.hideSelection) &&
          bimFilterState_.hideSelection) {
        bimFilterState_.isolateSelection = false;
      }
      if (!bimInspection.hasSelection) {
        bimFilterState_.isolateSelection = false;
        bimFilterState_.hideSelection = false;
        ImGui::EndDisabled();
      } else if (bimFilterState_.isolateSelection) {
        if (bimFilterState_.typeFilterEnabled &&
            bimFilterState_.type != bimInspection.type) {
          if (bimInspection.type.empty()) {
            bimFilterState_.typeFilterEnabled = false;
            bimFilterState_.type.clear();
          } else {
            bimFilterState_.type = bimInspection.type;
          }
        }
        const std::string selectedStorey = !bimInspection.storeyName.empty()
                                               ? bimInspection.storeyName
                                               : bimInspection.storeyId;
        if (bimFilterState_.storeyFilterEnabled &&
            bimFilterState_.storey != selectedStorey) {
          if (selectedStorey.empty()) {
            bimFilterState_.storeyFilterEnabled = false;
            bimFilterState_.storey.clear();
          } else {
            bimFilterState_.storey = selectedStorey;
          }
        }
        const std::string selectedMaterial =
            !bimInspection.materialName.empty()
                ? bimInspection.materialName
                : bimInspection.materialCategory;
        if (bimFilterState_.materialFilterEnabled &&
            bimFilterState_.material != selectedMaterial) {
          if (selectedMaterial.empty()) {
            bimFilterState_.materialFilterEnabled = false;
            bimFilterState_.material.clear();
          } else {
            bimFilterState_.material = selectedMaterial;
          }
        }
        auto syncSelectionFilter = [](bool &filterEnabled,
                                      std::string &filterValue,
                                      const std::string &selectedValue) {
          if (!filterEnabled || filterValue == selectedValue) {
            return;
          }
          if (selectedValue.empty()) {
            filterEnabled = false;
            filterValue.clear();
          } else {
            filterValue = selectedValue;
          }
        };
        syncSelectionFilter(bimFilterState_.disciplineFilterEnabled,
                            bimFilterState_.discipline,
                            bimInspection.discipline);
        syncSelectionFilter(bimFilterState_.phaseFilterEnabled,
                            bimFilterState_.phase, bimInspection.phase);
        syncSelectionFilter(bimFilterState_.fireRatingFilterEnabled,
                            bimFilterState_.fireRating,
                            bimInspection.fireRating);
        syncSelectionFilter(bimFilterState_.loadBearingFilterEnabled,
                            bimFilterState_.loadBearing,
                            bimInspection.loadBearing);
        syncSelectionFilter(bimFilterState_.statusFilterEnabled,
                            bimFilterState_.status, bimInspection.status);
      }
      if (bimFilterState_.hideSelection && bimFilterState_.isolateSelection) {
        bimFilterState_.hideSelection = false;
      }
      if (bimFilterState_.typeFilterEnabled && bimFilterState_.type.empty()) {
        bimFilterState_.typeFilterEnabled = false;
      }
      if (bimFilterState_.storeyFilterEnabled &&
          bimFilterState_.storey.empty()) {
        bimFilterState_.storeyFilterEnabled = false;
      }
      if (bimFilterState_.materialFilterEnabled &&
          bimFilterState_.material.empty()) {
        bimFilterState_.materialFilterEnabled = false;
      }
      if (bimFilterState_.disciplineFilterEnabled &&
          bimFilterState_.discipline.empty()) {
        bimFilterState_.disciplineFilterEnabled = false;
      }
      if (bimFilterState_.phaseFilterEnabled && bimFilterState_.phase.empty()) {
        bimFilterState_.phaseFilterEnabled = false;
      }
      if (bimFilterState_.fireRatingFilterEnabled &&
          bimFilterState_.fireRating.empty()) {
        bimFilterState_.fireRatingFilterEnabled = false;
      }
      if (bimFilterState_.loadBearingFilterEnabled &&
          bimFilterState_.loadBearing.empty()) {
        bimFilterState_.loadBearingFilterEnabled = false;
      }
      if (bimFilterState_.statusFilterEnabled &&
          bimFilterState_.status.empty()) {
        bimFilterState_.statusFilterEnabled = false;
      }

      if (ImGui::TreeNode("Layer Visibility")) {
        bool layerStateChanged = false;
        layerStateChanged |=
            ImGui::Checkbox("Point-cloud visibility",
                            &bimLayerVisibilityState_.pointCloudVisible);
        layerStateChanged |= ImGui::Checkbox(
            "Curve visibility", &bimLayerVisibilityState_.curvesVisible);
        ImGui::Separator();
        layerStateChanged |=
            ImGui::Checkbox("Clash markers",
                            &bimLayerVisibilityState_.clashLayerVisible);
        layerStateChanged |=
            ImGui::Checkbox("Issue markers",
                            &bimLayerVisibilityState_.markupLayerVisible);
        if (layerStateChanged) {
          statusMessage_ = "Layer visibility updated";
        }
        ImGui::Text("Active issue pins: %zu", bimIssueOverlayPins().size());
        ImGui::TextDisabled(
            "Point-cloud and curve toggles mask their BIM draw lists");
        ImGui::TreePop();
      }

      if (ImGui::TreeNode("LOD / Streaming")) {
        ImGui::Text("Loaded BIM objects: %zu", bimInspection.objectCount);
        ImGui::Text("Mesh / point / curve objects: %zu / %zu / %zu",
                    bimInspection.meshObjectCount,
                    bimInspection.pointObjectCount,
                    bimInspection.curveObjectCount);
        ImGui::Text("Visible draw lists: opaque %zu, transparent %zu",
                    bimInspection.opaqueDrawCount,
                    bimInspection.transparentDrawCount);
        ImGui::Text("Point draws: %zu / %zu",
                    bimInspection.pointOpaqueDrawCount,
                    bimInspection.pointTransparentDrawCount);
        ImGui::Text("Curve draws: %zu / %zu",
                    bimInspection.curveOpaqueDrawCount,
                    bimInspection.curveTransparentDrawCount);
        ImGui::Text("Native point draws: %zu / %zu",
                    bimInspection.nativePointOpaqueDrawCount,
                    bimInspection.nativePointTransparentDrawCount);
        ImGui::Text("Native curve draws: %zu / %zu",
                    bimInspection.nativeCurveOpaqueDrawCount,
                    bimInspection.nativeCurveTransparentDrawCount);
        ImGui::Text("Meshlet clusters: %zu (%zu source, %zu estimated)",
                    bimInspection.meshletClusterCount,
                    bimInspection.meshletSourceClusterCount,
                    bimInspection.meshletEstimatedClusterCount);
        ImGui::Text("Cluster refs / max LOD: %zu / %u",
                    bimInspection.meshletObjectReferenceCount,
                    bimInspection.meshletMaxLodLevel);
        ImGui::Text(
            "GPU residency: %zu objects, %zu clusters, %.2f MB, compute %s%s",
            bimInspection.meshletGpuResidentObjectCount,
            bimInspection.meshletGpuResidentClusterCount,
            static_cast<double>(bimInspection.meshletGpuBufferBytes) /
                (1024.0 * 1024.0),
            bimInspection.meshletGpuComputeReady ? "ready" : "offline",
            bimInspection.meshletGpuDispatchPending ? ", pending" : "");
        ImGui::Text("Optimized metadata cacheable: %s",
                    bimInspection.optimizedModelMetadataCacheable ? "yes"
                                                                  : "no");
        ImGui::Text(
            "Optimized metadata cache: %s",
            !bimInspection.optimizedModelMetadataCacheStatus.empty()
                ? bimInspection.optimizedModelMetadataCacheStatus.c_str()
            : bimInspection.optimizedModelMetadataCacheHit     ? "hit"
            : bimInspection.optimizedModelMetadataCacheWritten ? "written"
                                                               : "not written");
        if (!bimInspection.optimizedModelMetadataCacheKey.empty()) {
          TextWrappedPathValue("Cache key",
                               bimInspection.optimizedModelMetadataCacheKey);
        }
        if (!bimInspection.optimizedModelMetadataCachePath.empty()) {
          TextWrappedPathValue("Cache path",
                               bimInspection.optimizedModelMetadataCachePath);
        }
        ImGui::Text("Floor plan draws: %zu", bimInspection.floorPlanDrawCount);
        ImGui::Separator();
        ImGui::Checkbox("Auto LOD request", &bimLodStreamingUiState_.autoLod);
        ImGui::Checkbox("Draw budget enabled",
                        &bimLodStreamingUiState_.drawBudgetEnabled);
        if (!bimLodStreamingUiState_.drawBudgetEnabled) {
          ImGui::BeginDisabled();
        }
        ImGui::SliderInt("Max visible BIM objects",
                         &bimLodStreamingUiState_.drawBudgetMaxObjects, 100,
                         200000);
        if (!bimLodStreamingUiState_.drawBudgetEnabled) {
          ImGui::EndDisabled();
        }
        ImGui::SliderInt("LOD bias", &bimLodStreamingUiState_.lodBias, -4, 4);
        ImGui::SliderFloat("Screen error pixels",
                           &bimLodStreamingUiState_.screenErrorPixels, 0.25f,
                           16.0f, "%.2f");
        ImGui::Checkbox("Pause streaming request",
                        &bimLodStreamingUiState_.pauseStreamingRequest);
        ImGui::Checkbox("Keep visible storeys resident",
                        &bimLodStreamingUiState_.keepVisibleStoreysResident);
        bimLodStreamingUiState_.drawBudgetMaxObjects = std::clamp(
            bimLodStreamingUiState_.drawBudgetMaxObjects, 100, 200000);
        bimFilterState_.drawBudgetEnabled =
            bimLodStreamingUiState_.drawBudgetEnabled;
        bimFilterState_.drawBudgetMaxObjects =
            bimLodStreamingUiState_.drawBudgetEnabled
                ? static_cast<uint32_t>(
                      bimLodStreamingUiState_.drawBudgetMaxObjects)
                : 0u;
        ImGui::Text("Budget visible: %zu objects (%zu mesh), %zu cluster refs",
                    bimInspection.drawBudgetVisibleObjectCount,
                    bimInspection.drawBudgetVisibleMeshObjectCount,
                    bimInspection.drawBudgetVisibleMeshletClusterCount);
        ImGui::Text("Budget max LOD: %u",
                    bimInspection.drawBudgetVisibleMaxLodLevel);
        ImGui::TextDisabled(
            "Draw budget preserves BIM object identity; meshlet residency buffers are GPU-side and cacheable for streaming decisions");
        ImGui::TreePop();
      }

      ImGui::Separator();
      ImGui::Checkbox("Floor plan overlay", &bimFloorPlanOverlayState_.enabled);
      if (bimFloorPlanOverlayState_.enabled) {
        static constexpr const char *kFloorPlanElevationModeLabels[] = {
            "Projected on ground", "Source elevation"};
        int floorPlanElevationMode =
            static_cast<int>(bimFloorPlanOverlayState_.elevationMode);
        if (ImGui::Combo("Floor plan elevation", &floorPlanElevationMode,
                         kFloorPlanElevationModeLabels,
                         IM_ARRAYSIZE(kFloorPlanElevationModeLabels))) {
          bimFloorPlanOverlayState_.elevationMode =
              static_cast<BimFloorPlanElevationMode>(
                  std::clamp(floorPlanElevationMode, 0, 1));
        }
        ImGui::Checkbox("Floor plan depth test",
                        &bimFloorPlanOverlayState_.depthTest);
        ImGui::ColorEdit3("Floor plan color",
                          &bimFloorPlanOverlayState_.color.x);
        ImGui::SliderFloat("Floor plan opacity",
                           &bimFloorPlanOverlayState_.opacity, 0.05f, 1.0f,
                           "%.2f");
        if (wireframeWideLineSupported_) {
          ImGui::SliderFloat("Floor plan line width",
                             &bimFloorPlanOverlayState_.lineWidth, 1.0f, 8.0f,
                             "%.1f");
        } else {
          bimFloorPlanOverlayState_.lineWidth = 1.0f;
          ImGui::BeginDisabled();
          ImGui::SliderFloat("Floor plan line width",
                             &bimFloorPlanOverlayState_.lineWidth, 1.0f, 1.0f,
                             "%.1f");
          ImGui::EndDisabled();
        }
        bimFloorPlanOverlayState_.opacity =
            std::clamp(bimFloorPlanOverlayState_.opacity, 0.05f, 1.0f);
        bimFloorPlanOverlayState_.lineWidth =
            std::max(bimFloorPlanOverlayState_.lineWidth, 1.0f);
      }

      ImGui::Separator();
      if (ImGui::TreeNode("Clip / Cap / Hatching")) {
        ImGui::Checkbox("Section plane", &sectionPlaneState_.enabled);
        static constexpr const char *kSectionAxisLabels[] = {"X", "Y", "Z",
                                                             "Custom"};
        int sectionAxisCombo = sectionPlaneAxis_ < 0 ? 3 : sectionPlaneAxis_;
        if (ImGui::Combo("Section axis", &sectionAxisCombo, kSectionAxisLabels,
                         IM_ARRAYSIZE(kSectionAxisLabels))) {
          switch (sectionAxisCombo) {
          case 0:
            sectionPlaneAxis_ = 0;
            sectionPlaneState_.normal = {1.0f, 0.0f, 0.0f};
            break;
          case 2:
            sectionPlaneAxis_ = 2;
            sectionPlaneState_.normal = {0.0f, 0.0f, 1.0f};
            break;
          case 3:
            sectionPlaneAxis_ = -1;
            break;
          default:
            sectionPlaneAxis_ = 1;
            sectionPlaneState_.normal = {0.0f, 1.0f, 0.0f};
            break;
          }
        }
        if (sectionPlaneAxis_ < 0) {
          if (ImGui::DragFloat3("Section normal", &sectionPlaneState_.normal.x,
                                0.01f, -1.0f, 1.0f, "%.3f")) {
            const float length = glm::length(sectionPlaneState_.normal);
            sectionPlaneState_.normal = length > 0.0001f
                                            ? sectionPlaneState_.normal / length
                                            : glm::vec3{0.0f, 1.0f, 0.0f};
          }
        }
        ImGui::SliderFloat("Section offset", &sectionPlaneState_.offset,
                           -100.0f, 100.0f, "%.3f");
        ImGui::Checkbox("Show cut plane",
                        &sectionPlaneState_.visualPlaneVisible);
        if (!sectionPlaneState_.enabled ||
            !sectionPlaneState_.visualPlaneVisible) {
          sectionPlaneState_.visualPlaneEditable = false;
        }
        if (!sectionPlaneState_.enabled ||
            !sectionPlaneState_.visualPlaneVisible) {
          ImGui::BeginDisabled();
        }
        ImGui::Checkbox("Edit cut plane as object",
                        &sectionPlaneState_.visualPlaneEditable);
        glm::vec3 sectionNormal =
            glm::length(sectionPlaneState_.normal) > 0.0001f
                ? glm::normalize(sectionPlaneState_.normal)
                : glm::vec3{0.0f, 1.0f, 0.0f};
        glm::vec3 cutPlaneOrigin = sectionNormal * sectionPlaneState_.offset;
        if (ImGui::DragFloat3("Cut plane origin", &cutPlaneOrigin.x, 0.05f,
                              -100000.0f, 100000.0f, "%.3f")) {
          sectionPlaneState_.offset = glm::dot(sectionNormal, cutPlaneOrigin);
        }
        ImGui::DragFloat("Cut plane size",
                         &sectionPlaneState_.visualPlaneSize, 0.1f, 0.1f,
                         100000.0f, "%.2f");
        ImGui::ColorEdit3("Cut plane color",
                          &sectionPlaneState_.visualPlaneColor.x);
        ImGui::SliderFloat("Cut plane opacity",
                           &sectionPlaneState_.visualPlaneOpacity, 0.05f, 1.0f,
                           "%.2f");
        ImGui::SliderFloat("Cut plane line width",
                           &sectionPlaneState_.visualPlaneLineWidth, 0.5f,
                           8.0f, "%.1f");
        if (!sectionPlaneState_.enabled ||
            !sectionPlaneState_.visualPlaneVisible) {
          ImGui::EndDisabled();
        }
        if (ImGui::SmallButton("Flip section")) {
          sectionPlaneState_.normal = -sectionPlaneState_.normal;
          sectionPlaneState_.offset = -sectionPlaneState_.offset;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset section")) {
          sectionPlaneState_ = {};
          sectionPlaneAxis_ = 1;
        }
        sectionPlaneState_.visualPlaneSize =
            std::clamp(sectionPlaneState_.visualPlaneSize, 0.1f, 100000.0f);
        sectionPlaneState_.visualPlaneOpacity =
            std::clamp(sectionPlaneState_.visualPlaneOpacity, 0.05f, 1.0f);
        sectionPlaneState_.visualPlaneLineWidth =
            std::clamp(sectionPlaneState_.visualPlaneLineWidth, 0.5f, 8.0f);
        ImGui::Separator();
        ImGui::Checkbox("Box clip", &bimBoxClipState_.enabled);
        if (bimBoxClipState_.enabled) {
          ImGui::Checkbox("Invert box clip", &bimBoxClipState_.invert);
          ImGui::DragFloat3("Box min", &bimBoxClipState_.min.x, 0.1f, -10000.0f,
                            10000.0f, "%.3f");
          ImGui::DragFloat3("Box max", &bimBoxClipState_.max.x, 0.1f, -10000.0f,
                            10000.0f, "%.3f");
          for (int axis = 0; axis < 3; ++axis) {
            if (bimBoxClipState_.min[axis] > bimBoxClipState_.max[axis]) {
              std::swap(bimBoxClipState_.min[axis], bimBoxClipState_.max[axis]);
            }
          }
          if (ImGui::SmallButton("Reset box clip")) {
            bimBoxClipState_ = {};
          }
          ImGui::Separator();
        }
        ImGui::Checkbox("Section cap fill",
                        &bimClipCapHatchingUiState_.capPreview);
        if (!bimClipCapHatchingUiState_.capPreview) {
          ImGui::BeginDisabled();
        }
        ImGui::ColorEdit3("Cap fill color",
                          &bimClipCapHatchingUiState_.capColor.x);
        ImGui::SliderFloat("Cap fill opacity",
                           &bimClipCapHatchingUiState_.capOpacity, 0.05f, 1.0f,
                           "%.2f");
        if (!bimClipCapHatchingUiState_.capPreview) {
          ImGui::EndDisabled();
        }
        ImGui::Checkbox("Section hatch overlay",
                        &bimClipCapHatchingUiState_.hatchingPreview);
        if (!bimClipCapHatchingUiState_.hatchingPreview) {
          ImGui::BeginDisabled();
        }
        ImGui::SliderFloat("Hatch spacing",
                           &bimClipCapHatchingUiState_.hatchSpacing, 0.05f,
                           5.0f, "%.2f");
        ImGui::SliderFloat("Hatch angle degrees",
                           &bimClipCapHatchingUiState_.hatchAngleDegrees, 0.0f,
                           180.0f, "%.1f");
        ImGui::SliderFloat("Hatch line width",
                           &bimClipCapHatchingUiState_.hatchLineWidth, 1.0f,
                           8.0f, "%.1f");
        ImGui::ColorEdit3("Hatch color",
                          &bimClipCapHatchingUiState_.hatchColor.x);
        if (!bimClipCapHatchingUiState_.hatchingPreview) {
          ImGui::EndDisabled();
        }
        ImGui::Checkbox("Per-material cut styles",
                        &bimClipCapHatchingUiState_.perMaterialCutStyles);
        if (!bimClipCapHatchingUiState_.perMaterialCutStyles) {
          ImGui::BeginDisabled();
        }
        int concreteMaterialIndex = static_cast<int>(
            bimClipCapHatchingUiState_.concreteMaterialIndex);
        int glassMaterialIndex =
            static_cast<int>(bimClipCapHatchingUiState_.glassMaterialIndex);
        ImGui::InputInt("Concrete material index", &concreteMaterialIndex);
        ImGui::SliderFloat("Concrete hatch spacing",
                           &bimClipCapHatchingUiState_.concreteHatchSpacing,
                           0.05f, 1.0f, "%.2f");
        ImGui::InputInt("Glass material index", &glassMaterialIndex);
        ImGui::SliderFloat("Glass hatch spacing",
                           &bimClipCapHatchingUiState_.glassHatchSpacing, 0.05f,
                           2.0f, "%.2f");
        bimClipCapHatchingUiState_.concreteMaterialIndex =
            static_cast<uint32_t>(std::max(concreteMaterialIndex, 0));
        bimClipCapHatchingUiState_.glassMaterialIndex =
            static_cast<uint32_t>(std::max(glassMaterialIndex, 0));
        if (!bimClipCapHatchingUiState_.perMaterialCutStyles) {
          ImGui::EndDisabled();
        }
        ImGui::Checkbox("Section marker arrows",
                        &bimClipCapHatchingUiState_.sectionMarkersPreview);
        if (!bimClipCapHatchingUiState_.sectionMarkersPreview) {
          ImGui::BeginDisabled();
        }
        ImGui::ColorEdit3("Section marker color",
                          &bimClipCapHatchingUiState_.sectionMarkerColor.x);
        ImGui::SliderFloat("Section marker line width",
                           &bimClipCapHatchingUiState_.sectionMarkerLineWidth,
                           0.5f, 8.0f, "%.1f");
        if (!bimClipCapHatchingUiState_.sectionMarkersPreview) {
          ImGui::EndDisabled();
        }
        bimClipCapHatchingUiState_.hatchSpacing =
            std::clamp(bimClipCapHatchingUiState_.hatchSpacing, 0.05f, 5.0f);
        bimClipCapHatchingUiState_.hatchAngleDegrees = std::clamp(
            bimClipCapHatchingUiState_.hatchAngleDegrees, 0.0f, 180.0f);
        bimClipCapHatchingUiState_.capOpacity =
            std::clamp(bimClipCapHatchingUiState_.capOpacity, 0.05f, 1.0f);
        bimClipCapHatchingUiState_.hatchLineWidth =
            std::clamp(bimClipCapHatchingUiState_.hatchLineWidth, 1.0f, 8.0f);
        bimClipCapHatchingUiState_.concreteHatchSpacing = std::clamp(
            bimClipCapHatchingUiState_.concreteHatchSpacing, 0.05f, 1.0f);
        bimClipCapHatchingUiState_.glassHatchSpacing =
            std::clamp(bimClipCapHatchingUiState_.glassHatchSpacing, 0.05f,
                       2.0f);
        bimClipCapHatchingUiState_.sectionMarkerLineWidth = std::clamp(
            bimClipCapHatchingUiState_.sectionMarkerLineWidth, 0.5f, 8.0f);
        ImGui::TextDisabled("Section caps rebuild when the section plane or "
                            "hatch style changes");
        ImGui::TreePop();
      }

      ImGui::Separator();
      if (ImGui::TreeNode("Measurements")) {
        int snapModeIndex = static_cast<int>(bimMeasurementSnapState_.mode);
        if (ImGui::Combo("Snap mode", &snapModeIndex,
                         kBimMeasurementSnapModeLabels.data(),
                         static_cast<int>(
                             kBimMeasurementSnapModeLabels.size()))) {
          snapModeIndex =
              std::clamp(snapModeIndex, 0,
                         static_cast<int>(
                             kBimMeasurementSnapModeLabels.size()) -
                             1);
          bimMeasurementSnapState_.mode =
              static_cast<BimMeasurementSnapMode>(snapModeIndex);
        }
        const bool snapEnabled =
            bimMeasurementSnapState_.mode != BimMeasurementSnapMode::Off;
        if (!snapEnabled) {
          ImGui::BeginDisabled();
        }
        ImGui::SliderFloat("Snap radius (px)",
                           &bimMeasurementSnapState_.maxScreenDistancePixels,
                           1.0f, 64.0f, "%.0f");
        if (!snapEnabled) {
          ImGui::EndDisabled();
        }
        bimMeasurementSnapState_.maxScreenDistancePixels = std::clamp(
            bimMeasurementSnapState_.maxScreenDistancePixels, 1.0f, 64.0f);
        ImGui::Text("Active snap: %s",
                    BimMeasurementSnapModeLabel(
                        bimMeasurementSnapState_.mode));
        ImGui::Separator();

        if (bimInspection.hasSelectionBounds) {
          const glm::vec3 dimensions =
              BimMeasurementDimensions(bimInspection.selectionBoundsSize);
          ImGui::TextUnformatted("Selected element");
          ImGui::Text("Dimensions (X/Y/Z): %.3f / %.3f / %.3f", dimensions.x,
                      dimensions.y, dimensions.z);
          ImGui::Text("Footprint (X*Z): %.3f",
                      BimMeasurementFootprintArea(dimensions));
          ImGui::Text("Volume (bounds): %.3f",
                      BimMeasurementVolume(dimensions));
          ImGui::Text("Center: %.3f, %.3f, %.3f",
                      bimInspection.selectionBoundsCenter.x,
                      bimInspection.selectionBoundsCenter.y,
                      bimInspection.selectionBoundsCenter.z);
          ImGui::Text("Live floor elevation: %.3f",
                      bimInspection.selectionFloorElevation);
        } else if (bimInspection.hasSelection) {
          ImGui::TextDisabled("Selected element has no bounds");
        } else {
          ImGui::TextDisabled("No selected BIM element");
        }

        ImGui::Separator();
        ImGui::TextUnformatted("Measurement points");
        auto captureMeasurementPoint = [&](BimMeasurementPointState &point) {
          const std::optional<BimMeasurementCapturedPoint> captured =
              container::ui::CaptureBimMeasurementPointFromSelection(
                  bimInspection, bimMeasurementSnapState_);
          if (!captured.has_value()) {
            return false;
          }
          point.captured = true;
          point.center = captured->center;
          point.objectIndex = captured->objectIndex;
          point.label = captured->label;
          point.modelPath = captured->modelPath;
          point.snapKind = captured->snapKind;
          return true;
        };
        const bool canCaptureMeasurement = bimInspection.hasSelectionBounds;
        if (!canCaptureMeasurement) {
          ImGui::BeginDisabled();
        }
        if (ImGui::SmallButton("Set A from selection")) {
          if (captureMeasurementPoint(bimMeasurementPointA_)) {
            statusMessage_ =
                "Set BIM measurement A from " + bimMeasurementPointA_.label;
          }
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Set B from selection")) {
          if (captureMeasurementPoint(bimMeasurementPointB_)) {
            statusMessage_ =
                "Set BIM measurement B from " + bimMeasurementPointB_.label;
          }
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Set C from selection")) {
          if (captureMeasurementPoint(bimMeasurementPointC_)) {
            statusMessage_ =
                "Set BIM measurement C from " + bimMeasurementPointC_.label;
          }
        }
        if (!canCaptureMeasurement) {
          ImGui::EndDisabled();
        }
        ImGui::SameLine();
        const bool hasMeasurementPoint =
            bimMeasurementPointA_.captured || bimMeasurementPointB_.captured ||
            bimMeasurementPointC_.captured;
        if (!hasMeasurementPoint) {
          ImGui::BeginDisabled();
        }
        if (ImGui::SmallButton("Clear measurement")) {
          bimMeasurementPointA_ = {};
          bimMeasurementPointB_ = {};
          bimMeasurementPointC_ = {};
          statusMessage_ = "Cleared BIM measurement";
        }
        if (!hasMeasurementPoint) {
          ImGui::EndDisabled();
        }

        auto drawMeasurementPoint = [](const char *name,
                                       const BimMeasurementPointState &point) {
          if (!point.captured) {
            ImGui::TextDisabled("%s: not set", name);
            return;
          }
          ImGui::TextWrapped("%s: %s", name, point.label.c_str());
          ImGui::Text("  Object: %u  Point: %.3f, %.3f, %.3f",
                      point.objectIndex, point.center.x, point.center.y,
                      point.center.z);
        };
        drawMeasurementPoint("A", bimMeasurementPointA_);
        drawMeasurementPoint("B", bimMeasurementPointB_);
        drawMeasurementPoint("C", bimMeasurementPointC_);

        if (bimMeasurementPointA_.captured && bimMeasurementPointB_.captured) {
          const container::renderer::BimMeasurementResult measurement =
              container::renderer::ComputeBimMeasurement(
                  bimMeasurementPointA_.center, bimMeasurementPointB_.center);
          const float elevationAxisAngleDegrees =
              BimMeasurementElevationAxisAngleDegrees(measurement);
          float angleDegrees = 0.0f;
          float polygonArea = 0.0f;
          if (bimMeasurementPointC_.captured) {
            angleDegrees = container::renderer::ComputeBimAngleDegrees(
                bimMeasurementPointA_.center, bimMeasurementPointB_.center,
                bimMeasurementPointC_.center);
            const std::array<glm::vec3, 3> polygon{
                bimMeasurementPointA_.center, bimMeasurementPointB_.center,
                bimMeasurementPointC_.center};
            polygonArea = container::renderer::ComputeBimPolygonArea(
                polygon, {0.0f, 0.0f, 0.0f});
          }
          ImGui::Text("Distance: %.3f", measurement.distance);
          ImGui::Text("Horizontal distance: %.3f",
                      measurement.horizontalDistance);
          ImGui::Text("Elevation delta: %.3f", measurement.elevationDelta);
          ImGui::Text("Slope angle: %.2f deg", measurement.angleDegrees);
          ImGui::Text("Angle to elevation axis: %.2f deg",
                      elevationAxisAngleDegrees);
          if (bimMeasurementPointC_.captured) {
            ImGui::Text("Angle A-B-C: %.2f deg", angleDegrees);
            ImGui::Text("Polygon area A/B/C: %.3f", polygonArea);
          } else {
            ImGui::TextDisabled("Angle A-B-C: --");
            ImGui::TextDisabled("Polygon area A/B/C: --");
          }
          if (ImGui::SmallButton("Save measurement annotation")) {
            BimMeasurementAnnotationState annotation{};
            annotation.id = nextBimMeasurementAnnotationId_++;
            annotation.label = "Measurement " + std::to_string(annotation.id);
            annotation.pointA = bimMeasurementPointA_;
            annotation.pointB = bimMeasurementPointB_;
            annotation.pointC = bimMeasurementPointC_;
            annotation.hasPointC = bimMeasurementPointC_.captured;
            annotation.distance = measurement.distance;
            annotation.horizontalDistance = measurement.horizontalDistance;
            annotation.elevationDelta = measurement.elevationDelta;
            annotation.slopeAngleDegrees = measurement.angleDegrees;
            annotation.elevationAxisAngleDegrees = elevationAxisAngleDegrees;
            annotation.angleDegrees = angleDegrees;
            annotation.polygonArea = polygonArea;
            bimMeasurementAnnotations_.push_back(std::move(annotation));
            selectedBimMeasurementAnnotationIndex_ =
                static_cast<int>(bimMeasurementAnnotations_.size()) - 1;
            statusMessage_ = "Saved BIM measurement annotation";
          }
        } else {
          ImGui::TextDisabled("Set A and B from selected BIM centers");
        }

        ImGui::Separator();
        ImGui::TextUnformatted("Measurement annotations");
        if (bimMeasurementAnnotations_.empty()) {
          selectedBimMeasurementAnnotationIndex_ = -1;
          ImGui::TextDisabled("No saved measurement annotations");
        } else {
          if (selectedBimMeasurementAnnotationIndex_ >=
              static_cast<int>(bimMeasurementAnnotations_.size())) {
            selectedBimMeasurementAnnotationIndex_ =
                static_cast<int>(bimMeasurementAnnotations_.size()) - 1;
          }
          if (ImGui::SmallButton("Clear annotations")) {
            bimMeasurementAnnotations_.clear();
            selectedBimMeasurementAnnotationIndex_ = -1;
            statusMessage_ = "Cleared BIM measurement annotations";
          }
          int eraseAnnotationIndex = -1;
          if (!bimMeasurementAnnotations_.empty() &&
              ImGui::BeginTable("BimMeasurementAnnotations", 8,
                                ImGuiTableFlags_Borders |
                                    ImGuiTableFlags_RowBg |
                                    ImGuiTableFlags_SizingStretchProp,
                                ImVec2(0.0f, 180.0f))) {
            ImGui::TableSetupColumn("Name");
            ImGui::TableSetupColumn("Distance");
            ImGui::TableSetupColumn("Horizontal");
            ImGui::TableSetupColumn("Elevation");
            ImGui::TableSetupColumn("Axis angle");
            ImGui::TableSetupColumn("Angle");
            ImGui::TableSetupColumn("Area");
            ImGui::TableSetupColumn("Action");
            ImGui::TableHeadersRow();
            for (size_t i = 0; i < bimMeasurementAnnotations_.size(); ++i) {
              const BimMeasurementAnnotationState &annotation =
                  bimMeasurementAnnotations_[i];
              ImGui::PushID(static_cast<int>(i));
              ImGui::TableNextRow();
              ImGui::TableNextColumn();
              const bool selected =
                  selectedBimMeasurementAnnotationIndex_ == static_cast<int>(i);
              if (ImGui::Selectable(annotation.label.c_str(), selected,
                                    ImGuiSelectableFlags_SpanAllColumns)) {
                selectedBimMeasurementAnnotationIndex_ = static_cast<int>(i);
              }
              std::string annotationTooltip =
                  annotation.pointA.label + " to " + annotation.pointB.label;
              if (annotation.hasPointC) {
                annotationTooltip += " to " + annotation.pointC.label;
              }
              ShowItemTooltip(annotationTooltip);
              ImGui::TableNextColumn();
              ImGui::Text("%.3f", annotation.distance);
              ImGui::TableNextColumn();
              ImGui::Text("%.3f", annotation.horizontalDistance);
              ImGui::TableNextColumn();
              ImGui::Text("%.3f", annotation.elevationDelta);
              ImGui::TableNextColumn();
              ImGui::Text("%.2f deg", annotation.elevationAxisAngleDegrees);
              ImGui::TableNextColumn();
              if (annotation.hasPointC) {
                ImGui::Text("%.2f deg", annotation.angleDegrees);
              } else {
                ImGui::TextDisabled("--");
              }
              ImGui::TableNextColumn();
              if (annotation.hasPointC) {
                ImGui::Text("%.3f", annotation.polygonArea);
              } else {
                ImGui::TextDisabled("--");
              }
              ImGui::TableNextColumn();
              if (ImGui::SmallButton("Delete")) {
                eraseAnnotationIndex = static_cast<int>(i);
              }
              ImGui::PopID();
            }
            ImGui::EndTable();
          }
          if (eraseAnnotationIndex >= 0) {
            bimMeasurementAnnotations_.erase(
                bimMeasurementAnnotations_.begin() + eraseAnnotationIndex);
            if (bimMeasurementAnnotations_.empty()) {
              selectedBimMeasurementAnnotationIndex_ = -1;
            } else {
              selectedBimMeasurementAnnotationIndex_ = std::min<int>(
                  selectedBimMeasurementAnnotationIndex_,
                  static_cast<int>(bimMeasurementAnnotations_.size()) - 1);
            }
          }
        }
        ImGui::TreePop();
      }

      ImGui::Separator();
      if (ImGui::TreeNode("Viewpoints")) {
        if (ImGui::SmallButton("Capture viewpoint")) {
          ViewpointSnapshotState snapshot = currentViewpoint;
          snapshot.bimFilter = bimFilterState_;
          snapshot.phaseTimeline = bimPhaseTimelineUiState_;
          snapshot.bimModelPath = bimInspection.modelPath;
          snapshot.label =
              "Viewpoint " + std::to_string(nextViewpointSnapshotId_++);
          const std::string selectionLabel =
              ViewpointSnapshotSelectionLabel(snapshot);
          if (selectionLabel != "None") {
            snapshot.label += " - " + selectionLabel;
          }
          viewpointSnapshots_.push_back(std::move(snapshot));
          selectedViewpointSnapshotIndex_ =
              static_cast<int>(viewpointSnapshots_.size()) - 1;
          statusMessage_ = "Captured " + ViewpointSnapshotDisplayName(
                                             viewpointSnapshots_.back(),
                                             viewpointSnapshots_.size() - 1u);
        }

        auto normalizedCurrentViewpoint = [&]() {
          ViewpointSnapshotState snapshot = currentViewpoint;
          snapshot.bimFilter = bimFilterState_;
          snapshot.phaseTimeline = bimPhaseTimelineUiState_;
          snapshot.bimModelPath = bimInspection.modelPath;
          if (snapshot.label.empty()) {
            snapshot.label = "Current viewpoint";
          }
          return snapshot;
        };
        auto saveBcfViewpoint = [&](const ViewpointSnapshotState &snapshot) {
          const std::filesystem::path path{bcfViewpointPath_};
          if (path.has_parent_path()) {
            std::error_code directoryError;
            std::filesystem::create_directories(path.parent_path(),
                                                directoryError);
            if (directoryError) {
              statusMessage_ = "Failed to create BCF viewpoint directory: " +
                               path.parent_path().string();
              return;
            }
          }
          statusMessage_ =
              bcf::SaveVisualizationInfo(snapshot, path)
                  ? "Exported BCF viewpoint: " + path.string()
                  : "Failed to export BCF viewpoint: " + path.string();
        };
        auto makeBcfTopicFolder = [&](const ViewpointSnapshotState &snapshot,
                                      uint32_t topicId) {
          bcf::BcfTopicFolder topic{};
          const std::string topicGuid =
              "container-topic-" + std::to_string(topicId);
          const std::string viewpointGuid =
              "container-viewpoint-" + std::to_string(topicId);
          topic.markup.topic.guid = topicGuid;
          topic.markup.topic.title = TrimAscii(bcfTopicTitleInput_).empty()
                                         ? "BIM issue"
                                         : TrimAscii(bcfTopicTitleInput_);
          topic.markup.topic.status = TrimAscii(bcfTopicStatusInput_).empty()
                                          ? "Open"
                                          : TrimAscii(bcfTopicStatusInput_);
          topic.markup.topic.priority = TrimAscii(bcfTopicPriorityInput_);
          topic.markup.topic.labels = SplitBcfTopicLabels(bcfTopicLabelsInput_);
          topic.markup.topic.tags = {"Container"};

          bcf::BcfTopicViewpoint viewpoint{};
          viewpoint.markup.guid = viewpointGuid;
          viewpoint.markup.viewpointFile = "viewpoint-0001.bcfv";
          viewpoint.snapshot = snapshot;
          if (bimInspection.hasSelection) {
            bcf::BcfPin pin{};
            pin.guid = bcf::StablePinGuid(bimInspection.guid,
                                          bimInspection.sourceId, topicGuid);
            pin.label = BimMeasurementSelectionLabel(bimInspection);
            pin.hasLocation = true;
            pin.location = bimInspection.hasSelectionBounds
                               ? bimInspection.selectionBoundsCenter
                               : snapshot.camera.position;
            pin.ifcGuid = bimInspection.guid;
            pin.sourceId = bimInspection.sourceId;
            viewpoint.markup.pins.push_back(std::move(pin));
          }
          topic.markup.viewpoints.push_back(viewpoint.markup);
          topic.viewpoints.push_back(std::move(viewpoint));

          const std::string commentText = TrimAscii(bcfTopicCommentInput_);
          if (!commentText.empty()) {
            bcf::BcfComment comment{};
            comment.guid = "container-comment-" + std::to_string(topicId);
            comment.author = "Container";
            comment.text = commentText;
            comment.viewpointGuid = viewpointGuid;
            topic.markup.comments.push_back(std::move(comment));
          }
          return topic;
        };
        auto storeBcfTopicArchiveEntry =
            [&](const bcf::BcfTopicFolder &topic,
                const std::filesystem::path &path) {
              BcfTopicArchiveEntryState entry{};
              entry.id = nextBcfTopicArchiveId_++;
              entry.label = topic.markup.topic.title.empty()
                                ? "BCF topic " + std::to_string(entry.id)
                                : topic.markup.topic.title;
              entry.status = topic.markup.topic.status;
              entry.priority = topic.markup.topic.priority;
              entry.path = path.empty() ? std::string{} : path.string();
              entry.issuePins =
                  container::renderer::exportBcfPinsAsIssueOverlayMarkers(
                      topic.markup);
              if (!topic.viewpoints.empty() &&
                  topic.viewpoints.front().snapshot) {
                entry.hasSnapshot = true;
                entry.snapshot = *topic.viewpoints.front().snapshot;
                if (entry.snapshot.label.empty()) {
                  entry.snapshot.label = entry.label;
                }
              }
              bcfTopicArchiveEntries_.push_back(std::move(entry));
              selectedBcfTopicArchiveIndex_ =
                  static_cast<int>(bcfTopicArchiveEntries_.size()) - 1;
              bcfIssueOverlayPins_ =
                  bcfTopicArchiveEntries_.back().issuePins;
            };

        ImGui::InputText("BCF viewpoint file", &bcfViewpointPath_);
        const bool hasBcfPath = !bcfViewpointPath_.empty();
        if (!hasBcfPath) {
          ImGui::BeginDisabled();
        }
        if (ImGui::SmallButton("Export current BCF")) {
          saveBcfViewpoint(normalizedCurrentViewpoint());
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Import BCF")) {
          const std::filesystem::path path{bcfViewpointPath_};
          if (auto snapshot = bcf::LoadVisualizationInfo(path)) {
            if (snapshot->label.empty()) {
              snapshot->label = "Imported viewpoint " +
                                std::to_string(nextViewpointSnapshotId_++);
            }
            viewpointSnapshots_.push_back(std::move(*snapshot));
            selectedViewpointSnapshotIndex_ =
                static_cast<int>(viewpointSnapshots_.size()) - 1;
            statusMessage_ = "Imported BCF viewpoint: " + path.string();
          } else {
            statusMessage_ = "Failed to import BCF viewpoint: " + path.string();
          }
        }
        if (!hasBcfPath) {
          ImGui::EndDisabled();
        }

        if (ImGui::TreeNode("BCF Topics")) {
          ImGui::InputText("BCF topic folder", &bcfTopicFolderPath_);
          ImGui::InputText("BCF topic archive file", &bcfTopicArchivePath_);
          ImGui::InputText("Topic title", &bcfTopicTitleInput_);
          ImGui::InputText("Topic status", &bcfTopicStatusInput_);
          ImGui::InputText("Topic priority", &bcfTopicPriorityInput_);
          ImGui::InputText("Topic labels", &bcfTopicLabelsInput_);
          ImGui::InputTextMultiline("Topic comment", &bcfTopicCommentInput_,
                                    ImVec2(0.0f, 64.0f));
          auto makeCurrentBcfTopic = [&]() {
            ViewpointSnapshotState snapshot = normalizedCurrentViewpoint();
            snapshot.selectedBimObjectIndex =
                bimInspection.hasSelection ? bimInspection.selectedObjectIndex
                                           : snapshot.selectedBimObjectIndex;
            snapshot.selectedBimGuid = bimInspection.hasSelection
                                           ? bimInspection.guid
                                           : snapshot.selectedBimGuid;
            snapshot.selectedBimType = bimInspection.hasSelection
                                           ? bimInspection.type
                                           : snapshot.selectedBimType;
            snapshot.selectedBimSourceId = bimInspection.hasSelection
                                               ? bimInspection.sourceId
                                               : snapshot.selectedBimSourceId;
            return makeBcfTopicFolder(snapshot, nextBcfTopicArchiveId_);
          };
          if (ImGui::SmallButton("Archive current topic")) {
            bcf::BcfTopicFolder topic = makeCurrentBcfTopic();
            storeBcfTopicArchiveEntry(topic, {});
            statusMessage_ = "Archived current BCF topic in UI memory";
          }
          ImGui::SameLine();
          const bool hasTopicFolderPath = !bcfTopicFolderPath_.empty();
          if (!hasTopicFolderPath) {
            ImGui::BeginDisabled();
          }
          if (ImGui::SmallButton("Save topic folder")) {
            const std::filesystem::path path{bcfTopicFolderPath_};
            bcf::BcfTopicFolder topic = makeCurrentBcfTopic();
            if (bcf::SaveTopicFolder(topic, path)) {
              storeBcfTopicArchiveEntry(topic, path);
              statusMessage_ = "Saved BCF topic folder: " + path.string();
            } else {
              statusMessage_ =
                  "Failed to save BCF topic folder: " + path.string();
            }
          }
          ImGui::SameLine();
          if (ImGui::SmallButton("Load topic folder")) {
            const std::filesystem::path path{bcfTopicFolderPath_};
            if (auto topic = bcf::LoadTopicFolder(path)) {
              storeBcfTopicArchiveEntry(*topic, path);
              statusMessage_ = "Loaded BCF topic folder: " + path.string();
            } else {
              statusMessage_ =
                  "Failed to load BCF topic folder: " + path.string();
            }
          }
          if (!hasTopicFolderPath) {
            ImGui::EndDisabled();
          }

          const bool hasTopicArchivePath = !bcfTopicArchivePath_.empty();
          if (!hasTopicArchivePath) {
            ImGui::BeginDisabled();
          }
          if (ImGui::SmallButton("Save topic archive")) {
            const std::filesystem::path path{bcfTopicArchivePath_};
            bcf::BcfTopicFolder topic = makeCurrentBcfTopic();
            if (bcf::SaveBcfArchive(topic, path)) {
              storeBcfTopicArchiveEntry(topic, path);
              statusMessage_ = "Saved BCF topic archive: " + path.string();
            } else {
              statusMessage_ =
                  "Failed to save BCF topic archive: " + path.string();
            }
          }
          ImGui::SameLine();
          if (ImGui::SmallButton("Load topic archive")) {
            const std::filesystem::path path{bcfTopicArchivePath_};
            if (auto topic = bcf::LoadBcfArchive(path)) {
              storeBcfTopicArchiveEntry(*topic, path);
              statusMessage_ = "Loaded BCF topic archive: " + path.string();
            } else {
              statusMessage_ =
                  "Failed to load BCF topic archive: " + path.string();
            }
          }
          if (!hasTopicArchivePath) {
            ImGui::EndDisabled();
          }

          if (bcfTopicArchiveEntries_.empty()) {
            selectedBcfTopicArchiveIndex_ = -1;
            bcfIssueOverlayPins_.clear();
            ImGui::TextDisabled("No archived BCF topics");
          } else {
            selectedBcfTopicArchiveIndex_ = std::clamp(
                selectedBcfTopicArchiveIndex_, 0,
                static_cast<int>(bcfTopicArchiveEntries_.size()) - 1);
            const BcfTopicArchiveEntryState &selectedTopic =
                bcfTopicArchiveEntries_[static_cast<size_t>(
                    selectedBcfTopicArchiveIndex_)];
            const std::string preview =
                selectedTopic.label.empty() ? "BCF topic" : selectedTopic.label;
            if (ImGui::BeginCombo("Archived topic", preview.c_str())) {
              for (size_t i = 0; i < bcfTopicArchiveEntries_.size(); ++i) {
                const BcfTopicArchiveEntryState &entry =
                    bcfTopicArchiveEntries_[i];
                const bool selected =
                    selectedBcfTopicArchiveIndex_ == static_cast<int>(i);
                std::string label = entry.label.empty()
                                        ? "BCF topic " + std::to_string(i + 1u)
                                        : entry.label;
                if (!entry.status.empty()) {
                  label += " [" + entry.status + "]";
                }
                if (ImGui::Selectable(label.c_str(), selected)) {
                  selectedBcfTopicArchiveIndex_ = static_cast<int>(i);
                  bcfIssueOverlayPins_ = entry.issuePins;
                }
                if (selected) {
                  ImGui::SetItemDefaultFocus();
                }
              }
              ImGui::EndCombo();
            }
            const BcfTopicArchiveEntryState &entry =
                bcfTopicArchiveEntries_[static_cast<size_t>(
                    selectedBcfTopicArchiveIndex_)];
            ImGui::TextWrapped("Topic status: %s", entry.status.empty()
                                                       ? "not specified"
                                                       : entry.status.c_str());
            ImGui::TextWrapped("Topic priority: %s",
                               entry.priority.empty() ? "not specified"
                                                      : entry.priority.c_str());
            if (!entry.path.empty()) {
              TextWrappedPathValue("Topic path", entry.path);
            }
            if (!entry.hasSnapshot) {
              ImGui::BeginDisabled();
            }
            if (ImGui::SmallButton("Restore archived topic viewpoint")) {
              const bool modelMismatch =
                  ViewpointSnapshotModelMismatch(entry.snapshot, bimInspection);
              const BimFilterState previousFilter = bimFilterState_;
              const BimPhaseTimelineUiState previousTimeline =
                  bimPhaseTimelineUiState_;
              if (!modelMismatch) {
                bimFilterState_ = entry.snapshot.bimFilter;
                bimPhaseTimelineUiState_ = entry.snapshot.phaseTimeline;
              }
              const bool restored =
                  restoreViewpoint ? restoreViewpoint(entry.snapshot) : false;
              if (!restored) {
                bimFilterState_ = previousFilter;
                bimPhaseTimelineUiState_ = previousTimeline;
              }
              statusMessage_ =
                  restored
                      ? (modelMismatch
                             ? "Restored archived BCF topic camera; kept current BIM filters"
                                  : "Restored archived BCF topic viewpoint")
                      : "Failed to restore archived BCF topic viewpoint";
            }
            if (!entry.hasSnapshot) {
              ImGui::EndDisabled();
              ImGui::TextDisabled(
                  "Archived BCF topic has no viewpoint snapshot");
            }
          }
          ImGui::TextDisabled(
              "BCF topic archive hooks are local file import/export; issue server sync is not connected");
          ImGui::TreePop();
        }

        if (viewpointSnapshots_.empty()) {
          selectedViewpointSnapshotIndex_ = -1;
          ImGui::TextDisabled("No captured viewpoints");
        } else {
          selectedViewpointSnapshotIndex_ =
              std::clamp(selectedViewpointSnapshotIndex_, 0,
                         static_cast<int>(viewpointSnapshots_.size()) - 1);
          const std::string preview = ViewpointSnapshotDisplayName(
              viewpointSnapshots_[static_cast<size_t>(
                  selectedViewpointSnapshotIndex_)],
              static_cast<size_t>(selectedViewpointSnapshotIndex_));
          if (ImGui::BeginCombo("Stored viewpoint", preview.c_str())) {
            for (size_t i = 0; i < viewpointSnapshots_.size(); ++i) {
              const bool selected =
                  static_cast<int>(i) == selectedViewpointSnapshotIndex_;
              const std::string label =
                  ViewpointSnapshotDisplayName(viewpointSnapshots_[i], i);
              if (ImGui::Selectable(label.c_str(), selected)) {
                selectedViewpointSnapshotIndex_ = static_cast<int>(i);
              }
              if (selected) {
                ImGui::SetItemDefaultFocus();
              }
            }
            ImGui::EndCombo();
          }

          ViewpointSnapshotState &selectedSnapshot =
              viewpointSnapshots_[static_cast<size_t>(
                  selectedViewpointSnapshotIndex_)];
          const bool modelMismatch =
              ViewpointSnapshotModelMismatch(selectedSnapshot, bimInspection);
          ImGui::InputText("Viewpoint name", &selectedSnapshot.label);
          if (modelMismatch) {
            ImGui::TextColored(ImVec4(1.0f, 0.65f, 0.2f, 1.0f),
                               "Snapshot model differs");
            TextWrappedPathValue("Snapshot source",
                                 selectedSnapshot.bimModelPath);
          }
          ImGui::Text("Camera: %.2f, %.2f, %.2f",
                      selectedSnapshot.camera.position.x,
                      selectedSnapshot.camera.position.y,
                      selectedSnapshot.camera.position.z);
          ImGui::Text("Rotation: %.1f, %.1f",
                      selectedSnapshot.camera.rotationDegrees.x,
                      selectedSnapshot.camera.rotationDegrees.y);
          ImGui::TextWrapped(
              "Selection: %s",
              ViewpointSnapshotSelectionLabel(selectedSnapshot).c_str());
          if (selectedSnapshot.bimFilter.typeFilterEnabled &&
              !selectedSnapshot.bimFilter.type.empty()) {
            ImGui::TextWrapped("Filter: %s",
                               selectedSnapshot.bimFilter.type.c_str());
          }
          if (selectedSnapshot.bimFilter.isolateSelection) {
            ImGui::TextDisabled("Isolate selected");
          }

          if (ImGui::Button("Restore viewpoint")) {
            const BimFilterState previousFilter = bimFilterState_;
            const BimPhaseTimelineUiState previousTimeline =
                bimPhaseTimelineUiState_;
            if (!modelMismatch) {
              bimFilterState_ = selectedSnapshot.bimFilter;
              bimPhaseTimelineUiState_ = selectedSnapshot.phaseTimeline;
            }
            const bool restored =
                restoreViewpoint ? restoreViewpoint(selectedSnapshot) : false;
            if (!restored) {
              bimFilterState_ = previousFilter;
              bimPhaseTimelineUiState_ = previousTimeline;
            }
            const std::string restoredName = ViewpointSnapshotDisplayName(
                selectedSnapshot,
                static_cast<size_t>(selectedViewpointSnapshotIndex_));
            statusMessage_ =
                restored
                    ? (modelMismatch ? "Restored camera from " + restoredName +
                                           "; kept current BIM filters"
                                     : "Restored " + restoredName)
                    : "Failed to restore viewpoint";
          }
          ImGui::SameLine();
          if (!hasBcfPath) {
            ImGui::BeginDisabled();
          }
          if (ImGui::SmallButton("Export selected BCF")) {
            saveBcfViewpoint(selectedSnapshot);
          }
          if (!hasBcfPath) {
            ImGui::EndDisabled();
          }
          ImGui::SameLine();
          if (ImGui::SmallButton("Delete viewpoint")) {
            const size_t removedIndex =
                static_cast<size_t>(selectedViewpointSnapshotIndex_);
            const std::string removedName = ViewpointSnapshotDisplayName(
                viewpointSnapshots_[removedIndex], removedIndex);
            viewpointSnapshots_.erase(viewpointSnapshots_.begin() +
                                      selectedViewpointSnapshotIndex_);
            if (viewpointSnapshots_.empty()) {
              selectedViewpointSnapshotIndex_ = -1;
            } else {
              selectedViewpointSnapshotIndex_ = std::min<int>(
                  selectedViewpointSnapshotIndex_,
                  static_cast<int>(viewpointSnapshots_.size()) - 1);
            }
            statusMessage_ = "Deleted " + removedName;
          }
        }
        ImGui::TreePop();
      }

      if (!bimInspection.elementTypes.empty() &&
          ImGui::TreeNode("Element Types")) {
        const size_t visibleTypeCount =
            std::min<size_t>(bimInspection.elementTypes.size(), 48u);
        for (size_t i = 0; i < visibleTypeCount; ++i) {
          ImGui::BulletText("%s", bimInspection.elementTypes[i].c_str());
        }
        if (visibleTypeCount < bimInspection.elementTypes.size()) {
          ImGui::TextDisabled("%zu more", bimInspection.elementTypes.size() -
                                              visibleTypeCount);
        }
        ImGui::TreePop();
      }

      ImGui::Separator();
      if (bimInspection.hasSelection) {
        auto drawIndexLine = [](const char *label, uint32_t value) {
          if (value == std::numeric_limits<uint32_t>::max()) {
            ImGui::TextDisabled("%s: n/a", label);
          } else {
            ImGui::Text("%s: %u", label, value);
          }
        };
        auto drawTextLine = [](const char *label, const std::string &value) {
          if (!value.empty()) {
            ImGui::TextWrapped("%s: %s", label, value.c_str());
          }
        };
        auto drawVec3Line = [](const char *label, const glm::vec3 &value) {
          ImGui::Text("%s: %.3f, %.3f, %.3f", label, value.x, value.y, value.z);
        };
        ImGui::Text("Selected BIM Object: %u",
                    bimInspection.selectedObjectIndex);
        drawTextLine("Name", bimInspection.displayName);
        if (!bimInspection.type.empty()) {
          ImGui::TextWrapped("Type: %s", bimInspection.type.c_str());
        }
        drawTextLine("Object type", bimInspection.objectType);
        if (!bimInspection.guid.empty()) {
          ImGui::TextWrapped("GUID: %s", bimInspection.guid.c_str());
        }
        drawTextLine("Source ID", bimInspection.sourceId);
        drawTextLine("Storey", bimInspection.storeyName);
        drawTextLine("Storey ID", bimInspection.storeyId);
        drawTextLine("Material", bimInspection.materialName);
        drawTextLine("Material category", bimInspection.materialCategory);
        drawTextLine("Discipline", bimInspection.discipline);
        drawTextLine("Phase", bimInspection.phase);
        drawTextLine("Fire rating", bimInspection.fireRating);
        drawTextLine("Load-bearing", bimInspection.loadBearing);
        drawTextLine("Status", bimInspection.status);
        drawTextLine("Geometry", bimInspection.geometryKind);
        drawIndexLine("Source element", bimInspection.sourceElementIndex);
        drawIndexLine("Mesh", bimInspection.meshId);
        drawIndexLine("Source material", bimInspection.sourceMaterialIndex);
        drawIndexLine("Render material", bimInspection.materialIndex);
        drawIndexLine("Semantic type", bimInspection.semanticTypeId);
        ImGui::Text("Transparent: %s",
                    bimInspection.transparent ? "yes" : "no");
        ImGui::Text("Double-sided: %s",
                    bimInspection.doubleSided ? "yes" : "no");
        if (bimInspection.hasSelectionBounds) {
          ImGui::Separator();
          ImGui::TextUnformatted("Bounds");
          drawVec3Line("Min", bimInspection.selectionBoundsMin);
          drawVec3Line("Max", bimInspection.selectionBoundsMax);
          drawVec3Line("Center", bimInspection.selectionBoundsCenter);
          drawVec3Line("Size", bimInspection.selectionBoundsSize);
          ImGui::Text("Radius: %.3f", bimInspection.selectionBoundsRadius);
          ImGui::Text("Floor elevation: %.3f",
                      bimInspection.selectionFloorElevation);
        }
        glm::vec4 color = bimInspection.sourceColor;
        ImGui::BeginDisabled();
        ImGui::ColorEdit4("Source color", &color.x);
        ImGui::EndDisabled();
      } else {
        ImGui::TextDisabled("No BIM selection");
      }
      ImGui::TreePop();
    }
  }

  if (cullStatsTotal_ > 0) {
    ImGui::Separator();
    ImGui::Text("GPU Culling");
    ImGui::Checkbox("Freeze culling camera (F8)", &freezeCulling_);
    if (freezeCulling_)
      ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "CULLING FROZEN");
    ImGui::BulletText("Input: %u", cullStatsTotal_);
    ImGui::BulletText("Frustum passed: %u", cullStatsFrustum_);
    ImGui::BulletText("Occlusion passed: %u", cullStatsOcclusion_);
    ImGui::BulletText("Frustum culled: %u",
                      cullStatsTotal_ - cullStatsFrustum_);
    if (cullStatsFrustum_ > 0)
      ImGui::BulletText("Occlusion culled: %u",
                        cullStatsFrustum_ - cullStatsOcclusion_);
  }

  if (!renderTechniqueDebugState_.panels.empty()) {
    ImGui::Separator();
    const std::string title =
        renderTechniqueDebugState_.displayName.empty()
            ? std::string{"Render Technique"}
            : "Render Technique: " + renderTechniqueDebugState_.displayName;
    if (ImGui::TreeNode(title.c_str())) {
      for (const GuiRenderTechniqueDebugPanel &panel :
           renderTechniqueDebugState_.panels) {
        const char *panelTitle =
            panel.title.empty() ? panel.id.c_str() : panel.title.c_str();
        if (ImGui::TreeNode(panelTitle)) {
          for (const GuiRenderTechniqueDebugControl &control :
               panel.controls) {
            ImGui::BulletText("%s", control.label.c_str());
            ShowItemTooltip(control.kind);
          }
          ImGui::TreePop();
        }
      }
      ImGui::TreePop();
    }
  }

  if (!renderPassToggles_.empty()) {
    ImGui::Separator();
    if (ImGui::TreeNode("Render Passes")) {
      if (ImGui::SmallButton("Enable All")) {
        for (auto &p : renderPassToggles_)
          p.enabled = true;
      }
      ImGui::SameLine();
      if (ImGui::SmallButton("Disable All")) {
        // Keep PostProcess always on so the UI remains visible.
        for (auto &p : renderPassToggles_) {
          p.enabled = p.locked;
        }
      }

      auto drawRenderPassSection = [&](std::string_view sectionName) {
        if (!ImGui::TreeNode(std::string(sectionName).c_str())) {
          return;
        }

        for (auto &p : renderPassToggles_) {
          if (RenderPassSectionName(p.name) == sectionName) {
            DrawRenderPassToggleEntry(p);
          }
        }

        ImGui::TreePop();
      };

      drawRenderPassSection("Culling");
      drawRenderPassSection("Shadows");
      drawRenderPassSection("Lighting");
      drawRenderPassSection("Post-process");

      ShowItemTooltip(
          "Protected passes are shown as locked. Optional passes may be "
          "auto-disabled when a dependency is off.");
      ImGui::TreePop();
    }
  }

  ImGui::Separator();
  ImGui::Text("Post-process");
  int exposureMode =
      exposureSettings_.mode == container::gpu::kExposureModeAuto ? 1 : 0;
  static constexpr const char *kExposureModeLabels[] = {"Manual", "Auto"};
  if (ImGui::Combo("Exposure Mode", &exposureMode, kExposureModeLabels,
                   IM_ARRAYSIZE(kExposureModeLabels))) {
    exposureSettings_.mode = exposureMode == 1
                                 ? container::gpu::kExposureModeAuto
                                 : container::gpu::kExposureModeManual;
  }
  ImGui::SliderFloat("Exposure", &exposureSettings_.manualExposure, 0.0f, 4.0f,
                     "%.3f");
  if (exposureSettings_.mode == container::gpu::kExposureModeAuto) {
    ImGui::SliderFloat("Target Luminance", &exposureSettings_.targetLuminance,
                       0.01f, 2.0f, "%.3f");
    ImGui::SliderFloat("Min Exposure", &exposureSettings_.minExposure, 0.0f,
                       4.0f, "%.3f");
    ImGui::SliderFloat("Max Exposure", &exposureSettings_.maxExposure, 0.0f,
                       16.0f, "%.3f");
    exposureSettings_.maxExposure =
        std::max(exposureSettings_.maxExposure, exposureSettings_.minExposure);
    ImGui::SliderFloat("Adaptation Rate", &exposureSettings_.adaptationRate,
                       0.0f, 10.0f, "%.2f");
    ImGui::SliderFloat("Low Percentile",
                       &exposureSettings_.meteringLowPercentile, 0.0f, 0.99f,
                       "%.2f");
    ImGui::SliderFloat(
        "High Percentile", &exposureSettings_.meteringHighPercentile,
        exposureSettings_.meteringLowPercentile + 0.01f, 1.0f, "%.2f");
    exposureSettings_.meteringHighPercentile =
        std::max(exposureSettings_.meteringHighPercentile,
                 exposureSettings_.meteringLowPercentile + 0.01f);
  }
  if (ImGui::TreeNode("Temporal anti-aliasing")) {
    ImGui::BeginDisabled(msaaSamples_ != 1u);
    ImGui::Checkbox("Enable TAA", &temporalSettings_.enabled);
    ImGui::EndDisabled();
    if (msaaSamples_ != 1u)
      ImGui::TextDisabled("Select MSAA 1x to enable TAA.");
    ImGui::SliderFloat("History weight", &temporalSettings_.historyWeight, 0.0f,
                       0.98f);
    ImGui::SliderFloat("Variance clipping", &temporalSettings_.varianceGamma,
                       0.5f, 4.0f);
    ImGui::SliderFloat("Depth tolerance (world units)",
                       &temporalSettings_.depthAbsoluteTolerance, 0.0f, 0.1f,
                       "%.4f");
    ImGui::SliderFloat("Relative depth tolerance",
                       &temporalSettings_.depthRelativeTolerance, 0.0f, 0.05f,
                       "%.4f");
    ImGui::Text("Submitted frame %llu, epoch %u",
                static_cast<unsigned long long>(temporalFrame_),
                temporalEpoch_);
    ImGui::Text("Image payload: %.1f MiB",
                double(temporalBytes_) / (1024.0 * 1024.0));
    ImGui::Text("Effective AA: %s; raster/history %ux%u",
                temporalSettings_.enabled ? "TAA (1x)" : "MSAA / native raster",
                temporalExtent_.x, temporalExtent_.y);
    ImGui::Text("Allocated images: %.1f MiB; resolve GPU %.3f ms",
                double(temporalAllocatedBytes_) / (1024.0 * 1024.0),
                temporalResolveGpuMs_);
    ImGui::TextWrapped("Last reset: %s", temporalResetReason_.c_str());
    ImGui::TextWrapped("Transparent and emissive pixels use current color. TAA "
                       "runs before exposure, bloom and tone mapping.");
    ImGui::TextDisabled(
        "GPU times: TemporalVelocity / TemporalResolve in renderer telemetry.");
    if (ImGui::Button("Reset temporal history"))
      temporalResetRequested_ = true;
    ImGui::TreePop();
  }
  if (ImGui::TreeNode("Vulkan runtime capture")) {
    ImGui::TextWrapped("%s", gfxCaptureStatus_.c_str());
    ImGui::TextDisabled("Capture must be armed before instance creation.");
    ImGui::TreePop();
  }
  ImGui::Text("Bloom");
  ImGui::Checkbox("Bloom Enabled", &bloomEnabled_);
  if (bloomEnabled_) {
    ImGui::SliderFloat("Bloom Threshold", &bloomThreshold_, 0.0f, 5.0f);
    ImGui::SliderFloat("Bloom Knee", &bloomKnee_, 0.0f, 1.0f);
    ImGui::SliderFloat("Bloom Intensity", &bloomIntensity_, 0.0f, 2.0f);
    ImGui::SliderFloat("Bloom Radius", &bloomRadius_, 0.1f, 3.0f);
  }

  if (ImGui::TreeNode("Shadows")) {
    ImGui::TextWrapped("Ray queries: %s", rayShadowStatus_.c_str());
    ImGui::BeginDisabled(!rayShadowSupported_);
    int rayMode = static_cast<int>(rayShadowSettings_.mode);
    if (ImGui::Combo("Shadow visibility", &rayMode,
                     "Raster\0Ray hard\0Ray soft\0"))
      rayShadowSettings_.mode =
          static_cast<container::renderer::RayShadowMode>(rayMode);
    int raySamples = static_cast<int>(rayShadowSettings_.areaSamples);
    if (ImGui::SliderInt("Emitter rays", &raySamples, 1, 32))
      rayShadowSettings_.areaSamples = uint32_t(raySamples);
    int rayBudget = static_cast<int>(rayShadowSettings_.localLightBudget);
    if (ImGui::SliderInt("Traced local lights", &rayBudget, 0, 8))
      rayShadowSettings_.localLightBudget = uint32_t(rayBudget);
    ImGui::Checkbox("Denoise ray soft shadows", &rayShadowSettings_.denoise);
    int rayDebug = static_cast<int>(rayShadowSettings_.debugLayer);
    if (ImGui::SliderInt("Ray shadow debug view", &rayDebug, 0, 10))
      rayShadowSettings_.debugLayer = uint32_t(rayDebug);
    ImGui::TextDisabled(
        "Views: 0 shaded, 1 directional, 2-5 point/spot, 6-9 area, 10 budget.");
    ImGui::TextDisabled(
        "Budget RGB: emitter samples / 32, local lights / 8, directional.");
    ImGui::EndDisabled();
    ImGui::Text("Directional shadow atlas: %u x %u",
                container::gpu::kShadowMapResolution,
                container::gpu::kShadowMapResolution);
    ImGui::Text("Local shadow atlas: %u x %u",
                container::gpu::kLocalShadowMapResolution,
                container::gpu::kLocalShadowMapResolution);
    ImGui::SliderFloat("Normal Bias Min Texels",
                       &shadowSettings_.normalBiasMinTexels, 0.0f, 8.0f,
                       "%.2f");
    ImGui::SliderFloat("Normal Bias Max Texels",
                       &shadowSettings_.normalBiasMaxTexels, 0.0f, 12.0f,
                       "%.2f");
    shadowSettings_.normalBiasMaxTexels =
        std::max(shadowSettings_.normalBiasMaxTexels,
                 shadowSettings_.normalBiasMinTexels);
    ImGui::SliderFloat("Slope Bias Scale", &shadowSettings_.slopeBiasScale,
                       0.0f, 0.01f, "%.5f");
    ImGui::SliderFloat("Receiver Bias Scale",
                       &shadowSettings_.receiverPlaneBiasScale, 0.0f, 4.0f,
                       "%.2f");
    ImGui::SliderFloat("Filter Radius Texels",
                       &shadowSettings_.filterRadiusTexels, 0.25f, 3.0f,
                       "%.2f");
    ImGui::Checkbox("Directional PCSS",
                    &shadowSettings_.directionalPcssEnabled);
    if (shadowSettings_.directionalPcssEnabled) {
      ImGui::SliderFloat("Sun Radius Degrees",
                         &shadowSettings_.directionalPcssLightRadiusDegrees,
                         0.0f, 3.0f, "%.2f");
      ImGui::SliderFloat(
          "Blocker Search Texels",
          &shadowSettings_.directionalPcssBlockerSearchRadiusTexels, 1.0f,
          32.0f, "%.1f");
      ImGui::SliderFloat("Max PCSS Filter Texels",
                         &shadowSettings_.directionalPcssMaxFilterRadiusTexels,
                         shadowSettings_.filterRadiusTexels, 32.0f, "%.1f");
      shadowSettings_.directionalPcssMaxFilterRadiusTexels =
          std::max(shadowSettings_.directionalPcssMaxFilterRadiusTexels,
                   shadowSettings_.filterRadiusTexels);
    }
    ImGui::Checkbox("Directional contact shadows",
                    &shadowSettings_.directionalContactVisibility);
    if (shadowSettings_.directionalContactVisibility) {
      ImGui::SliderFloat("Contact Max Distance",
                         &shadowSettings_.directionalContactMaxDistance,
                         0.0f, 0.75f, "%.2f");
      ImGui::SliderFloat("Contact Thickness",
                         &shadowSettings_.directionalContactThickness, 0.0f,
                         0.25f, "%.3f");
      shadowSettings_.directionalContactThickness =
          std::min(shadowSettings_.directionalContactThickness,
                   shadowSettings_.directionalContactMaxDistance);
      ImGui::SliderFloat("Contact Fade Distance",
                         &shadowSettings_.directionalContactFadeDistance,
                         0.0f, 0.75f, "%.2f");
      shadowSettings_.directionalContactFadeDistance =
          std::clamp(shadowSettings_.directionalContactFadeDistance,
                     shadowSettings_.directionalContactThickness,
                     shadowSettings_.directionalContactMaxDistance);
    }
    ImGui::SliderFloat("Cascade Blend", &shadowSettings_.cascadeBlendFraction,
                       0.0f, 0.45f, "%.2f");
    ImGui::SliderFloat("Constant Depth Bias",
                       &shadowSettings_.constantDepthBias, 0.0f, 0.005f,
                       "%.5f");
    ImGui::SliderFloat("Max Depth Bias", &shadowSettings_.maxDepthBias, 0.0f,
                       0.02f, "%.5f");
    ImGui::SliderFloat("Raster Constant Bias",
                       &shadowSettings_.rasterConstantBias, -16.0f, 0.0f,
                       "%.2f");
    ImGui::SliderFloat("Raster Slope Bias", &shadowSettings_.rasterSlopeBias,
                       -8.0f, 0.0f, "%.2f");
    ImGui::Checkbox("Local contact visibility",
                    &shadowSettings_.localContactVisibility);
    int areaQuality = static_cast<int>(shadowSettings_.areaShadowQuality);
    if (ImGui::Combo("Area shadow quality", &areaQuality,
                     "Fixed origins\0Fast (16 taps)\0Balanced (32 taps)\0High "
                     "(64 taps)\0"))
      shadowSettings_.areaShadowQuality = static_cast<uint32_t>(areaQuality);
    int localShadowPointBudget =
        static_cast<int>(lightingSettings_.localShadowPointBudget);
    if (ImGui::SliderInt("Shadowed Point Light Budget",
                         &localShadowPointBudget, 0, 4)) {
      lightingSettings_.localShadowPointBudget =
          static_cast<uint32_t>(std::max(localShadowPointBudget, 0));
    }
    int localShadowLayerBudget =
        static_cast<int>(lightingSettings_.localShadowLayerBudget);
    if (ImGui::SliderInt(
            "Local Shadow Layer Budget", &localShadowLayerBudget, 0,
            static_cast<int>(container::gpu::kMaxShadowedLocalLightLayers))) {
      lightingSettings_.localShadowLayerBudget = static_cast<uint32_t>(
          std::clamp(localShadowLayerBudget, 0,
                     static_cast<int>(
                         container::gpu::kMaxShadowedLocalLightLayers)));
    }
    ImGui::TreePop();
  }

  TransformControls editableCameraTransform = cameraTransform;
  if (DrawTransformControls("Camera", editableCameraTransform)) {
    applyCameraTransform(editableCameraTransform);
  }

  TransformControls editableSceneTransform = sceneTransform;
  if (DrawTransformControls("Scene", editableSceneTransform)) {
    applySceneTransform(editableSceneTransform);
  }

  if (ImGui::TreeNode("Lights")) {
    ImGui::Text("Editable lights");
    if (ImGui::SmallButton("Add directional")) {
      addManualEditableLight(EditableLightType::Directional);
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Add point")) {
      addManualEditableLight(EditableLightType::Point);
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Add spot")) {
      addManualEditableLight(EditableLightType::Spot);
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Add area")) {
      addManualEditableLight(EditableLightType::Area);
    }

    const auto selectedEditableIt =
        std::ranges::find_if(editableLights, [&](const auto &light) {
          return light.id == selectedEditableLight;
        });
    const char *selectedEditableLabel =
        selectedEditableIt != editableLights.end()
            ? selectedEditableIt->label.c_str()
            : "None";
    if (ImGui::BeginCombo("Selected light", selectedEditableLabel)) {
      if (ImGui::Selectable("None",
                            selectedEditableIt == editableLights.end())) {
        selectEditableLight({});
      }
      for (const EditableLightEntity &light : editableLights) {
        const bool selected = light.id == selectedEditableLight;
        if (ImGui::Selectable(light.label.c_str(), selected)) {
          selectEditableLight(light.id);
        }
        const std::string lightTooltip =
            std::string(container::renderer::editableLightSourceLabel(
                light.source)) +
            " " + container::renderer::editableLightTypeLabel(light.type);
        ShowItemTooltip(lightTooltip);
        if (selected) {
          ImGui::SetItemDefaultFocus();
        }
      }
      ImGui::EndCombo();
    }

    if (selectedEditableIt != editableLights.end()) {
      EditableLightEntity editableLight = *selectedEditableIt;
      if (DrawEditableLightInspector(editableLight)) {
        updateEditableLight(editableLight);
      }
    } else if (editableLights.empty()) {
      ImGui::TextDisabled("No editable lights");
    }

    if (ImGui::TreeNode("Lighting Settings")) {
      ImGui::SliderFloat("Directional Intensity",
                         &lightingSettings_.directionalIntensity, 0.0f, 16.0f,
                         "%.2f");
      ImGui::SliderFloat("Environment Intensity",
                         &lightingSettings_.environmentIntensity, 0.0f, 16.0f,
                         "%.2f");
      ImGui::SliderFloat("Bounce Intensity", &lightingSettings_.bounceIntensity,
                         0.0f, 2.0f, "%.2f");
      ShowItemTooltip("Artistic fill without traced indirect visibility. "
                      "High values brighten occluded surfaces; use zero to "
                      "inspect direct-light shadows.");
      ImGui::TreePop();
    }

    if (ImGui::TreeNode("Light Runtime Stats")) {
      ImGui::BulletText("Submitted lights: %u",
                        lightCullingStats_.submittedLights);
      ImGui::BulletText("Active clusters: %u / %u",
                        lightCullingStats_.activeClusters,
                        lightCullingStats_.totalClusters);
      ImGui::BulletText("Max lights per cluster: %u",
                        lightCullingStats_.maxLightsPerCluster);
      ImGui::BulletText("Dropped light refs: %u",
                        lightCullingStats_.droppedLightReferences);
      if (lightCullingStats_.droppedLightReferences > 0u ||
          lightCullingStats_.maxLightsPerCluster >=
              container::gpu::kMaxLightsPerTile) {
        ImGui::TextColored(ImVec4(1.0f, 0.82f, 0.32f, 1.0f),
                           "Cluster saturated: reduce imported/manual light "
                           "ranges.");
      }
      ImGui::BulletText("Cluster cull GPU: %.3f ms",
                        lightCullingStats_.clusterCullMs);
      ImGui::BulletText("Clustered lighting GPU: %.3f ms",
                        lightCullingStats_.clusteredLightingMs);

      ImGui::Separator();
      ImGui::Text("Directional");
      ImGui::BulletText("Position: (%.2f, %.2f, %.2f)",
                        directionalLightPosition.x, directionalLightPosition.y,
                        directionalLightPosition.z);
      ImGui::BulletText("Direction: (%.2f, %.2f, %.2f)",
                        lightingData.directionalDirection.x,
                        lightingData.directionalDirection.y,
                        lightingData.directionalDirection.z);

      const uint32_t submittedPointLightCount = static_cast<uint32_t>(
          std::min<size_t>(pointLights.size(), UINT32_MAX));
      const uint32_t visiblePointLightCount =
          std::min(submittedPointLightCount, kMaxDeferredPointLights);
      ImGui::Text("Point lights: %u submitted, %u uploaded",
                  submittedPointLightCount, lightingData.pointLightCount);
      if (submittedPointLightCount > visiblePointLightCount) {
        ImGui::Text("Showing first %u SSBO lights", visiblePointLightCount);
      }
      for (uint32_t i = 0; i < visiblePointLightCount; ++i) {
        const auto &light = pointLights[i];
        ImGui::Separator();
        ImGui::Text("Point Light %u", i);
        ImGui::BulletText("Position: (%.2f, %.2f, %.2f)",
                          light.positionRadius.x, light.positionRadius.y,
                          light.positionRadius.z);
        ImGui::BulletText("Radius: %.2f", light.positionRadius.w);
      }
      ImGui::TreePop();
    }
    ImGui::TreePop();
  }

  const auto &renderableNodes = sceneGraph.renderableNodes();
  if (!renderableNodes.empty()) {
    uint32_t activeMeshNode = selectedMeshNode;
    const bool hasActiveMesh =
        std::ranges::contains(renderableNodes, activeMeshNode);
    if (!hasActiveMesh) {
      activeMeshNode = container::scene::SceneGraph::kInvalidNode;
    }

    std::string selectedLabel =
        activeMeshNode == container::scene::SceneGraph::kInvalidNode
            ? "None"
            : "Node " + std::to_string(activeMeshNode);
    if (activeMeshNode != container::scene::SceneGraph::kInvalidNode) {
      if (const auto *node = sceneGraph.getNode(activeMeshNode);
          node != nullptr &&
          node->primitiveIndex != container::scene::SceneGraph::kInvalidNode) {
        selectedLabel += " / Primitive " + std::to_string(node->primitiveIndex);
      }
    }

    if (ImGui::BeginCombo("Mesh", selectedLabel.c_str())) {
      const bool noneSelected =
          activeMeshNode == container::scene::SceneGraph::kInvalidNode;
      if (ImGui::Selectable("None", noneSelected)) {
        activeMeshNode = container::scene::SceneGraph::kInvalidNode;
        selectMeshNode(activeMeshNode);
      }
      if (noneSelected) {
        ImGui::SetItemDefaultFocus();
      }
      for (uint32_t nodeIndex : renderableNodes) {
        std::string label = "Node " + std::to_string(nodeIndex);
        if (const auto *node = sceneGraph.getNode(nodeIndex);
            node != nullptr && node->primitiveIndex !=
                                   container::scene::SceneGraph::kInvalidNode) {
          label += " / Primitive " + std::to_string(node->primitiveIndex);
        }
        const bool selected = nodeIndex == activeMeshNode;
        if (ImGui::Selectable(label.c_str(), selected)) {
          activeMeshNode = nodeIndex;
          selectMeshNode(nodeIndex);
        }
        if (selected) {
          ImGui::SetItemDefaultFocus();
        }
      }
      ImGui::EndCombo();
    }

    if (activeMeshNode != container::scene::SceneGraph::kInvalidNode) {
      TransformControls editableMeshTransform = meshTransform;
      if (DrawTransformControls("Mesh Transform", editableMeshTransform)) {
        applyMeshTransform(activeMeshNode, editableMeshTransform);
      }
    }
  }

  if (!statusMessage_.empty()) {
    const std::string statusText = WrapLongPathForImGui(statusMessage_);
    ImGui::TextWrapped("%s", statusText.c_str());
  }

  if (!environmentStatus_.empty()) {
    ImGui::Separator();
    ImGui::Text("Environment");
    const std::string environmentText = WrapLongPathForImGui(environmentStatus_);
    ImGui::TextWrapped("%s", environmentText.c_str());
  }

  ImGui::End();

  drawRendererTelemetryWindow();
}

void GuiManager::drawRendererTelemetryWindow() {
  if (!initialized_)
    return;

  const auto &telemetry = rendererTelemetry_;
  if (!ImGui::Begin("Renderer Telemetry")) {
    ImGui::End();
    return;
  }

  const auto &latest = telemetry.latest;
  if (!latest.valid) {
    ImGui::TextDisabled("No renderer telemetry captured yet");
    ImGui::End();
    return;
  }

  const float frameMs =
      TelemetryPhaseMs(latest, GuiRendererTelemetryPhase::Frame);
  const float fps = frameMs > 0.0f ? 1000.0f / frameMs : 0.0f;
  const float gpuKnownMs = latest.timing.gpuKnownMs;

  if (ImGui::BeginTable("TelemetrySummary", 4,
                        ImGuiTableFlags_BordersInnerV |
                            ImGuiTableFlags_SizingStretchSame)) {
    ImGui::TableNextColumn();
    ImGui::TextDisabled("FPS");
    ImGui::Text("%.1f", fps);
    ImGui::TableNextColumn();
    ImGui::TextDisabled("CPU frame");
    ImGui::Text("%.3f ms", frameMs);
    ImGui::TableNextColumn();
    ImGui::TextDisabled("GPU known");
    ImGui::Text("%.3f ms", gpuKnownMs);
    ImGui::TableNextColumn();
    ImGui::TextDisabled("CPU p95");
    ImGui::Text("%.3f ms", telemetry.summary.p95CpuFrameMs);
    ImGui::EndTable();
  }

  ImGui::Text("Frame: %llu  Image: %u  Slot: %u / %u",
              static_cast<unsigned long long>(latest.frameIndex),
              latest.imageIndex, latest.sync.frameSlot,
              latest.sync.maxFramesInFlight);
  ImGuiTextStringView("GPU timing source", latest.timing.gpuSource);
  ImGui::Text("GPU profiler: %s",
              latest.gpuProfiler.available ? "available" : "unavailable");
  if (latest.gpuProfiler.resultLatencyFrames > 0u) {
    ImGui::Text("GPU result latency: %u frame",
                latest.gpuProfiler.resultLatencyFrames);
  }
  if (!latest.gpuProfiler.status.empty()) {
    ImGui::TextWrapped("%s", latest.gpuProfiler.status.c_str());
  }
  ImGui::Text("Swapchain: %ux%u, %u images", latest.resources.swapchainWidth,
              latest.resources.swapchainHeight,
              latest.resources.swapchainImageCount);
  if (latest.sync.serializedConcurrency) {
    ImGui::TextColored(ImVec4(1.0f, 0.68f, 0.22f, 1.0f),
                       "Concurrency: serialized");
    if (!latest.sync.concurrencyReason.empty()) {
      ImGui::TextWrapped("%s", latest.sync.concurrencyReason.c_str());
    }
  } else {
    ImGui::Text("Concurrency: current frame fence");
  }

  if (!telemetry.history.empty()) {
    std::vector<float> cpuHistory;
    std::vector<float> gpuHistory;
    cpuHistory.reserve(telemetry.history.size());
    gpuHistory.reserve(telemetry.history.size());
    for (const auto &sample : telemetry.history) {
      cpuHistory.push_back(sample.cpuFrameMs);
      gpuHistory.push_back(sample.gpuKnownMs);
    }

    const float cpuScaleMax =
        std::max({16.67f, telemetry.summary.maxCpuFrameMs, frameMs});
    ImGui::PlotLines("CPU frame ms", cpuHistory.data(),
                     static_cast<int>(cpuHistory.size()), 0, nullptr, 0.0f,
                     cpuScaleMax, ImVec2(0.0f, 72.0f));
    const float gpuScaleMax =
        std::max({1.0f, telemetry.summary.maxGpuKnownMs, gpuKnownMs});
    ImGui::PlotLines("Known GPU ms", gpuHistory.data(),
                     static_cast<int>(gpuHistory.size()), 0, nullptr, 0.0f,
                     gpuScaleMax, ImVec2(0.0f, 72.0f));
  }

  if (ImGui::CollapsingHeader("CPU phases", ImGuiTreeNodeFlags_DefaultOpen)) {
    static constexpr std::array<GuiRendererTelemetryPhase, 12> kPhases{{
        GuiRendererTelemetryPhase::WaitForFrame,
        GuiRendererTelemetryPhase::Readbacks,
        GuiRendererTelemetryPhase::AcquireImage,
        GuiRendererTelemetryPhase::ImageFenceWait,
        GuiRendererTelemetryPhase::ResourceGrowth,
        GuiRendererTelemetryPhase::Gui,
        GuiRendererTelemetryPhase::SceneUpdate,
        GuiRendererTelemetryPhase::DescriptorUpdate,
        GuiRendererTelemetryPhase::CommandRecord,
        GuiRendererTelemetryPhase::QueueSubmit,
        GuiRendererTelemetryPhase::Screenshot,
        GuiRendererTelemetryPhase::Present,
    }};
    static constexpr std::array<const char *, kPhases.size()> kLabels{{
        "Frame wait",
        "Readbacks",
        "Acquire image",
        "Image fence",
        "Resource growth",
        "GUI",
        "Scene update",
        "Descriptors",
        "Command record",
        "Queue submit",
        "Screenshot",
        "Present",
    }};

    if (ImGui::BeginTable("TelemetryCpuPhases", 3,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_SizingStretchProp)) {
      ImGui::TableSetupColumn("Phase");
      ImGui::TableSetupColumn("CPU ms");
      ImGui::TableSetupColumn("Frame %");
      ImGui::TableHeadersRow();
      for (size_t i = 0; i < kPhases.size(); ++i) {
        const float phaseMs = TelemetryPhaseMs(latest, kPhases[i]);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(kLabels[i]);
        ImGui::TableNextColumn();
        ImGui::Text("%.3f", phaseMs);
        ImGui::TableNextColumn();
        ImGui::Text("%.1f%%", PercentOf(phaseMs, frameMs));
      }
      ImGui::EndTable();
    }
  }

  if (ImGui::CollapsingHeader("Workload and culling",
                              ImGuiTreeNodeFlags_DefaultOpen)) {
    ImGui::Text("Objects: %u / capacity %u", latest.workload.objectCount,
                latest.resources.objectBufferCapacity);
    ImGui::Text("Draws: %u total, %u opaque, %u transparent",
                latest.workload.totalDrawCount, latest.workload.opaqueDrawCount,
                latest.workload.transparentDrawCount);
    ImGui::Text("OIT nodes: %u", latest.resources.oitNodeCapacity);

    const uint32_t frustumCulled =
        latest.culling.inputCount >= latest.culling.frustumPassedCount
            ? latest.culling.inputCount - latest.culling.frustumPassedCount
            : 0u;
    const uint32_t occlusionCulled =
        latest.culling.frustumPassedCount >= latest.culling.occlusionPassedCount
            ? latest.culling.frustumPassedCount -
                  latest.culling.occlusionPassedCount
            : 0u;
    ImGui::Separator();
    ImGui::Text("Culling");
    ImGui::Text("Input: %u", latest.culling.inputCount);
    ImGui::Text("Frustum passed: %u, culled: %u",
                latest.culling.frustumPassedCount, frustumCulled);
    ImGui::Text("Occlusion passed: %u, culled: %u",
                latest.culling.occlusionPassedCount, occlusionCulled);

    ImGui::Separator();
    ImGui::Text("Lighting");
    ImGui::Text("Submitted lights: %u", latest.lightCulling.submittedLights);
    ImGui::Text("Active clusters: %u / %u", latest.lightCulling.activeClusters,
                latest.lightCulling.totalClusters);
    ImGui::Text("Max lights per cluster: %u",
                latest.lightCulling.maxLightsPerCluster);
    ImGui::Text("Dropped light refs: %u",
                latest.lightCulling.droppedLightReferences);
    ImGui::Text("Cluster cull GPU: %.3f ms", latest.lightCulling.clusterCullMs);
    ImGui::Text("Clustered lighting GPU: %.3f ms",
                latest.lightCulling.clusteredLightingMs);
  }

  if (ImGui::CollapsingHeader("Render graph", ImGuiTreeNodeFlags_DefaultOpen)) {
    ImGui::Text("Passes: %u total, %u enabled, %u active, %u skipped",
                latest.graph.totalPasses, latest.graph.enabledPasses,
                latest.graph.activePasses, latest.graph.skippedPasses);
    ImGui::Text("Timed passes: %u CPU, %u GPU", latest.graph.cpuTimedPasses,
                latest.graph.gpuTimedPasses);
    if (ImGui::BeginTable("TelemetryRenderGraph", 6,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_ScrollY,
                          ImVec2(0.0f, 220.0f))) {
      ImGui::TableSetupColumn("Pass");
      ImGui::TableSetupColumn("Enabled");
      ImGui::TableSetupColumn("State");
      ImGui::TableSetupColumn("CPU ms");
      ImGui::TableSetupColumn("Known GPU ms");
      ImGui::TableSetupColumn("Blocker");
      ImGui::TableHeadersRow();
      for (const auto &pass : latest.passes) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(pass.name.c_str());
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(pass.enabled ? "yes" : "no");
        ImGui::TableNextColumn();
        if (pass.active) {
          ImGui::TextColored(ImVec4(0.25f, 0.85f, 0.38f, 1.0f), "Active");
        } else {
          ImGui::TextColored(ImVec4(0.9f, 0.55f, 0.18f, 1.0f), "%s",
                             pass.status.c_str());
        }
        ImGui::TableNextColumn();
        ImGui::Text("%.3f", pass.cpuRecordMs);
        ImGui::TableNextColumn();
        if (pass.gpuTimed) {
          ImGui::Text("%.3f", pass.gpuKnownMs);
        } else {
          ImGui::TextDisabled("-");
        }
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(pass.blocker.c_str());
      }
      ImGui::EndTable();
    }
  }

  if (ImGui::CollapsingHeader("Synchronization")) {
    ImGui::Text(
        "Wait for frame: %.3f ms",
        TelemetryPhaseMs(latest, GuiRendererTelemetryPhase::WaitForFrame));
    ImGui::Text(
        "Image fence wait: %.3f ms",
        TelemetryPhaseMs(latest, GuiRendererTelemetryPhase::ImageFenceWait));
    ImGui::Text(
        "Acquire image: %.3f ms",
        TelemetryPhaseMs(latest, GuiRendererTelemetryPhase::AcquireImage));
    ImGui::Text("Present: %.3f ms",
                TelemetryPhaseMs(latest, GuiRendererTelemetryPhase::Present));
    ImGui::Text(
        "Swapchain recreates: %llu",
        static_cast<unsigned long long>(latest.sync.swapchainRecreateCount));
    ImGui::Text(
        "Device wait idle calls: %llu",
        static_cast<unsigned long long>(latest.sync.deviceWaitIdleCount));
  }

  ImGui::End();
}

bool GuiManager::isCapturingInput() const {
  return isCapturingMouse() || isCapturingKeyboard();
}

bool GuiManager::isCapturingMouse() const {
  if (!initialized_)
    return false;
  const ImGuiIO &io = ImGui::GetIO();
  return io.WantCaptureMouse;
}

bool GuiManager::isCapturingKeyboard() const {
  if (!initialized_)
    return false;
  const ImGuiIO &io = ImGui::GetIO();
  return io.WantCaptureKeyboard;
}

void GuiManager::setWireframeCapabilities(bool supported,
                                          bool rasterModeSupported,
                                          bool wideLineSupported) {
  wireframeSupported_ = supported;
  wireframeRasterModeSupported_ = supported && rasterModeSupported;
  wireframeWideLineSupported_ = wideLineSupported;
  if (!wireframeSupported_) {
    wireframeSettings_.enabled = false;
  }
  if (!wireframeWideLineSupported_) {
    wireframeSettings_.lineWidth = 1.0f;
  }
}

void GuiManager::setCullStats(uint32_t total, uint32_t frustumPassed,
                              uint32_t occlusionPassed) {
  cullStatsTotal_ = total;
  cullStatsFrustum_ = frustumPassed;
  cullStatsOcclusion_ = occlusionPassed;
}

void GuiManager::setLightCullingStats(
    const container::gpu::LightCullingStats &stats) {
  lightCullingStats_ = stats;
}

void GuiManager::setRendererTelemetry(
    const container::renderer::RendererTelemetryView &telemetry) {
  rendererTelemetry_ = GuiRendererTelemetry(telemetry);
}

void GuiManager::setLightingSettings(
    const container::gpu::LightingSettings &settings) {
  lightingSettings_ = settings;
}

void GuiManager::setFreezeCulling(bool frozen) { freezeCulling_ = frozen; }

void GuiManager::setBloomSettings(bool enabled, float threshold, float knee,
                                  float intensity, float radius) {
  bloomEnabled_ = enabled;
  bloomThreshold_ = threshold;
  bloomKnee_ = knee;
  bloomIntensity_ = intensity;
  bloomRadius_ = radius;
}

void GuiManager::setRenderPassList(
    const std::vector<RenderPassToggle> &passes) {
  // Rebuild the toggle list, preserving existing enabled states by name.
  std::vector<RenderPassToggle> updated;
  updated.reserve(passes.size());
  for (const auto &incoming : passes) {
    bool found = false;
    for (const auto &existing : renderPassToggles_) {
      if (existing.name == incoming.name) {
        RenderPassToggle merged = existing;
        merged.locked = incoming.locked;
        merged.autoDisabled = incoming.autoDisabled;
        merged.dependencyNote = incoming.dependencyNote;
        updated.push_back(std::move(merged));
        found = true;
        break;
      }
    }
    if (!found) {
      updated.push_back(incoming);
    }
  }
  renderPassToggles_ = std::move(updated);
}

void GuiManager::ensureInitialized() const {
  if (!initialized_) {
    throw std::runtime_error("GuiManager used before initialization");
  }
}

} // namespace container::ui
