#include "Container/renderer/debug/DebugUiPresenter.h"

#include "Container/renderer/core/RenderGraph.h"
#include "Container/utility/GuiManager.h"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace container::renderer {

namespace {

container::ui::RenderPassToggle* findRenderPassToggle(
    std::vector<container::ui::RenderPassToggle>& toggles,
    RenderPassId id) {
  const std::string_view name = renderPassName(id);
  for (auto& toggle : toggles) {
    if (toggle.name == name) return &toggle;
  }
  return nullptr;
}

const container::ui::RenderPassToggle* findRenderPassToggle(
    const std::vector<container::ui::RenderPassToggle>& toggles,
    RenderPassId id) {
  const std::string_view name = renderPassName(id);
  for (const auto& toggle : toggles) {
    if (toggle.name == name) return &toggle;
  }
  return nullptr;
}

std::string dependencyNoteForPass(
    const std::vector<container::ui::RenderPassToggle>& toggles,
    RenderPassId id) {
  for (RenderPassId dependencyId : renderPassDependencies(id)) {
    const auto* dependency = findRenderPassToggle(toggles, dependencyId);
    if (dependency != nullptr && !dependency->enabled) {
      return "requires " + std::string(renderPassName(dependencyId));
    }
  }
  return {};
}

std::string executionNoteForPass(const RenderGraph& graph, RenderPassId id) {
  if (id == RenderPassId::Invalid) return {};

  const auto* status = graph.executionStatus(id);
  if (status == nullptr || status->active) return {};

  switch (status->skipReason) {
    case RenderPassSkipReason::Disabled:
    case RenderPassSkipReason::None:
      return {};
    case RenderPassSkipReason::MissingPassDependency:
      return "inactive: requires " +
             std::string(renderPassName(status->blockingPass));
    case RenderPassSkipReason::MissingResource:
      return "inactive: missing " +
             std::string(renderResourceName(status->blockingResource));
    case RenderPassSkipReason::MissingRecordCallback:
      return "inactive: no recorder";
  }

  return {};
}

std::string techniqueDebugControlKindName(TechniqueDebugControlKind kind) {
  switch (kind) {
  case TechniqueDebugControlKind::Toggle:
    return "toggle";
  case TechniqueDebugControlKind::Integer:
    return "integer";
  case TechniqueDebugControlKind::Float:
    return "float";
  case TechniqueDebugControlKind::Enum:
    return "enum";
  case TechniqueDebugControlKind::Action:
    return "action";
  }
  return {};
}

bool enforceRenderPassDependencies(
    std::vector<container::ui::RenderPassToggle>& toggles) {
  bool changed = false;

  for (auto& toggle : toggles) {
    const RenderPassId id = renderPassIdFromName(toggle.name);
    if (isProtectedRenderPass(id) && !toggle.enabled) {
      toggle.enabled = true;
      changed = true;
    }
  }

  bool madeProgress = true;
  while (madeProgress) {
    madeProgress = false;

    for (auto& toggle : toggles) {
      if (!toggle.enabled) continue;
      const RenderPassId id = renderPassIdFromName(toggle.name);
      for (RenderPassId dependencyId : renderPassDependencies(id)) {
        const auto* dependency = findRenderPassToggle(toggles, dependencyId);
        if (dependency != nullptr && !dependency->enabled) {
          toggle.enabled = false;
          changed = true;
          madeProgress = true;
          break;
        }
      }
    }
  }

  return changed;
}

RenderGraphDebugModel renderGraphDebugModel(const RenderGraph& graph) {
  RenderGraphDebugModel debugModel = graph.debugModel();
  for (auto& pass : debugModel.passes) {
    const RenderPassId id = renderPassIdFromName(pass.passName);
    const std::string executionNote = executionNoteForPass(graph, id);
    pass.locked = isProtectedRenderPass(id);
    pass.autoDisabled = !executionNote.empty();
    pass.dependencyNote = executionNote;
  }
  return debugModel;
}

}  // namespace

void DebugUiPresenter::publishRenderGraphDebugModel(
    container::ui::GuiManager& guiManager,
    const RenderGraphDebugModel& debugModel) {
  std::vector<container::ui::RenderPassToggle> passList;
  passList.reserve(debugModel.passes.size());
  for (const auto& pass : debugModel.passes) {
    const RenderPassId id = renderPassIdFromName(pass.passName);
    const bool autoDisabled =
        pass.autoDisabled ||
        (!pass.active && !pass.skipReason.empty() && pass.skipReason != "None");
    container::ui::RenderPassToggle toggle{};
    toggle.name = pass.passName;
    toggle.enabled = pass.enabled;
    toggle.locked = pass.locked || isProtectedRenderPass(id);
    toggle.autoDisabled = autoDisabled;
    toggle.dependencyNote = !pass.dependencyNote.empty()
                                ? pass.dependencyNote
                                : (autoDisabled
                                       ? "inactive: " + pass.skipReason
                                       : std::string{});
    passList.push_back(std::move(toggle));
  }
  guiManager.setRenderPassList(passList);
}

void DebugUiPresenter::publishTechniqueDebugModel(
    container::ui::GuiManager& guiManager,
    const TechniqueDebugModel& debugModel) {
  container::ui::GuiRenderTechniqueDebugState state{};
  state.techniqueName = debugModel.techniqueName;
  state.displayName = debugModel.displayName;
  state.displayModes.reserve(debugModel.displayModes.size());
  for (const TechniqueDisplayModeOption& mode : debugModel.displayModes) {
    state.displayModes.push_back(
        {.id = mode.id, .label = mode.label, .value = mode.value});
  }

  state.panels.reserve(debugModel.panels.size());
  for (const TechniqueDebugPanel& panel : debugModel.panels) {
    container::ui::GuiRenderTechniqueDebugPanel uiPanel{};
    uiPanel.id = panel.id;
    uiPanel.title = panel.title;
    uiPanel.controls.reserve(panel.controls.size());
    for (const TechniqueDebugControl& control : panel.controls) {
      uiPanel.controls.push_back(
          {.id = control.id,
           .label = control.label,
           .kind = techniqueDebugControlKindName(control.kind)});
    }
    state.panels.push_back(std::move(uiPanel));
  }

  guiManager.setRenderTechniqueDebugState(std::move(state));
}

void DebugUiPresenter::publishRenderPasses(
    container::ui::GuiManager& guiManager,
    const RenderGraph& graph) {
  publishRenderGraphDebugModel(guiManager, renderGraphDebugModel(graph));
}

bool DebugUiPresenter::applyRenderPassToggles(
    container::ui::GuiManager& guiManager,
    RenderGraph& graph) {
  auto& toggles = guiManager.renderPassToggles();
  const bool correctedDependencies = enforceRenderPassDependencies(toggles);

  for (const auto& toggle : toggles) {
    graph.setPassEnabled(renderPassIdFromName(toggle.name), toggle.enabled);
  }
  for (auto& toggle : toggles) {
    const RenderPassId id = renderPassIdFromName(toggle.name);
    toggle.locked = isProtectedRenderPass(id);
    const std::string dependencyNote = dependencyNoteForPass(toggles, id);
    const std::string executionNote = executionNoteForPass(graph, id);
    toggle.autoDisabled = !dependencyNote.empty() || !executionNote.empty();
    toggle.dependencyNote =
        !dependencyNote.empty() ? dependencyNote : executionNote;
  }

  return correctedDependencies;
}

}  // namespace container::renderer
