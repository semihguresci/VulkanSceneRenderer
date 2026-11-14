#include "Container/renderer/core/RendererFrontend.h"

#include "Container/renderer/bim/BimDdgiRooms.h"
#include "Container/renderer/bim/BimManager.h"
#include "Container/renderer/lighting/DdgiManager.h"
#include "Container/renderer/temporal/TemporalCapture.h"
#include "Container/renderer/temporal/TemporalManager.h"
#include "Container/utility/GuiManager.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <stdexcept>

namespace container::renderer {
namespace {
template <class Vector>
nlohmann::json vectorJson(const Vector& value) {
  return {value.x, value.y, value.z};
}
} // namespace

struct RendererFrontend::DdgiBimRoomPlanResult {
  DdgiBimRoomPlan plan;
  std::string sourceUnits;
  std::optional<float> metersPerSourceUnit, effectiveImportScale;
};

bool RendererFrontend::DdgiBimRoomPlanning::snapshotMatches(const BimManager* current) const {
  return current && current == model && current->hasScene() &&
      geometryRevision == current->geometryRevision() &&
      metadataRevision == current->metadataRevision() &&
      transformRevision == current->transformRevision();
}

void RendererFrontend::startDdgiBimRoomPlan(uint32_t maxProbes, bool wait) {
  using Status = DdgiBimRoomPlanning::Status;
  pollDdgiBimRoomPlan();
  auto& planner = ddgiBimRoomPlanning_;
  if (planner.job.valid())
    throw std::runtime_error("Wait for the previous room plan to finish stopping");
  const auto* model = subs_.bimManager.get();
  const auto* gi = subs_.ddgiManager.get();
  if (!model || !model->hasScene())
    throw std::invalid_argument("Open a BIM model before planning room volumes");
  if (!gi || !gi->supported())
    throw std::invalid_argument("This device does not support DDGI room volumes");
  if (maxProbes < 27 || maxProbes > 65536)
    throw std::invalid_argument("Choose a room plan budget from 27 to 65536 probes");
  // Detach the current BIM room boxes on request. The worker never borrows
  // data from the live model or its current Vulkan resources.
  auto rooms = ExtractBimDdgiRooms(*model);
  planner.base = gi->settings();
  planner.sceneIdentity = ddgiActiveSceneIdentity();
  planner.model = model;
  planner.geometryRevision = model->geometryRevision();
  planner.metadataRevision = model->metadataRevision();
  planner.transformRevision = model->transformRevision();
  planner.result.reset();
  planner.stop = std::stop_source{};
  planner.job = std::async(std::launch::async,
      [base = planner.base, rooms = std::move(rooms), maxProbes, stop = planner.stop.get_token()]() mutable {
        auto plan = planDdgiBimRooms(base, rooms.rooms, {.maxDetailProbes = maxProbes}, stop);
        return std::make_shared<const DdgiBimRoomPlanResult>(DdgiBimRoomPlanResult{
            std::move(plan), std::move(rooms.sourceUnits), rooms.metersPerSourceUnit, rooms.effectiveImportScale});
      });
  planner.status = Status::Planning;
  planner.diagnostic = "Planning detail volumes for BIM rooms";
  if (wait) {
    pollDdgiBimRoomPlan(true);
    if (planner.status != Status::Preview) throw std::runtime_error(planner.diagnostic);
  }
}

void RendererFrontend::pollDdgiBimRoomPlan(bool wait) {
  using Status = DdgiBimRoomPlanning::Status;
  auto& planner = ddgiBimRoomPlanning_;
  if (planner.model && (!planner.snapshotMatches(subs_.bimManager.get()) ||
                        !subs_.ddgiManager ||
                        (planner.result && planner.status == Status::Applied
                            ? planner.result->plan.settings : planner.base) != subs_.ddgiManager->settings())) {
    planner.stop.request_stop();
    planner.model = nullptr;
    planner.result.reset();
    planner.status = Status::Rejected;
    planner.diagnostic = "The BIM model, placement, or DDGI settings changed. Preview a new room plan.";
  }
  if (!planner.job.valid() ||
      (!wait && planner.job.wait_for(std::chrono::seconds(0)) != std::future_status::ready)) return;
  try {
    auto result = planner.job.get();
    if (planner.status != Status::Planning) return;
    if (result->plan.cancelled) {
      discardDdgiBimRoomPlan();
      return;
    }
    planner.result = std::move(result);
    planner.status = Status::Preview;
    if (planner.result->plan.rooms.empty())
      planner.diagnostic = "No BIM room or space definitions were found. Use manual or selected-element fitting.";
    else if (planner.result->plan.volumes.empty())
      planner.diagnostic = "The plan has no new detail volumes. Review room coverage and the shared limits.";
    else
      planner.diagnostic = "Room plan ready. Apply it to update the probe volumes.";
  } catch (const std::exception& error) {
    if (planner.status == Status::Planning) {
      planner.status = Status::Rejected;
      planner.diagnostic = error.what();
    }
  }
}

void RendererFrontend::applyDdgiBimRoomPlan() {
  using Status = DdgiBimRoomPlanning::Status;
  pollDdgiBimRoomPlan();
  auto& planner = ddgiBimRoomPlanning_;
  if (planner.status == Status::Rejected)
    throw std::runtime_error(planner.diagnostic);
  if (planner.status != Status::Preview || !planner.result)
    throw std::invalid_argument("Preview a BIM room plan before applying it");
  if (planner.result->plan.volumes.empty())
    throw std::invalid_argument("The BIM room plan has no new detail volumes to apply");
  if (planner.sceneIdentity != ddgiActiveSceneIdentity())
    throw std::invalid_argument("The scene identity changed. Preview a new room plan.");
  subs_.ddgiManager->applyPlannedVolumes(planner.result->plan.settings);
  planner.status = Status::Applied;
  planner.diagnostic = "Applied the BIM room detail volumes";
  if (subs_.temporalManager)
    subs_.temporalManager->reset("DDGI BIM room plan applied");
  if (subs_.guiManager) {
    subs_.guiManager->acceptDdgiSceneFit(subs_.ddgiManager->settings(), planner.diagnostic);
    subs_.guiManager->selectDdgiVolume(planner.result->plan.volumes.front().volumeIndex);
  }
}

void RendererFrontend::discardDdgiBimRoomPlan() {
  auto& planner = ddgiBimRoomPlanning_;
  planner.stop.request_stop();
  planner.model = nullptr;
  planner.result.reset();
  planner.status = DdgiBimRoomPlanning::Status::Idle;
  planner.diagnostic.clear();
}

void RendererFrontend::publishDdgiBimRoomPlanUi() {
  if (!subs_.guiManager) return;
  using Status = DdgiBimRoomPlanning::Status;
  const auto& planner = ddgiBimRoomPlanning_;
  auto result = planner.result;
  subs_.guiManager->setDdgiRoomPlanUiState({
      .modelAvailable = subs_.bimManager && subs_.bimManager->hasScene(),
      .running = planner.job.valid(),
      .canApply = planner.status == Status::Preview && result && !result->plan.volumes.empty(),
      .result = result ? std::shared_ptr<const DdgiBimRoomPlan>(result, &result->plan) : nullptr,
      .diagnostic = planner.diagnostic});
}

void RendererFrontend::processDdgiBimRoomPlan() {
  if (!subs_.guiManager) return;
  using Action = container::ui::GuiManager::DdgiRoomPlanRequest::Action;
  if (const auto request = subs_.guiManager->consumeDdgiRoomPlanRequest()) {
    try {
      switch (request->action) {
      case Action::Preview: startDdgiBimRoomPlan(request->maxProbes); break;
      case Action::Apply: applyDdgiBimRoomPlan(); break;
      case Action::Discard: discardDdgiBimRoomPlan(); break;
      }
    } catch (const std::exception& error) {
      ddgiBimRoomPlanning_.status = DdgiBimRoomPlanning::Status::Rejected;
      ddgiBimRoomPlanning_.diagnostic = error.what();
    }
  }
}

void RendererFrontend::applyCaptureDdgiRoomPlan(const container::temporal::CaptureDdgiRoomPlan& event) {
  using Action = container::temporal::CaptureDdgiRoomPlan::Action;
  try {
    switch (event.action) {
    case Action::Preview: startDdgiBimRoomPlan(event.maxDetailProbes, true); break;
    case Action::Apply: applyDdgiBimRoomPlan(); break;
    case Action::Discard: discardDdgiBimRoomPlan(); break;
    }
  } catch (const std::exception& error) {
    if (!event.expectFailure) throw;
    ddgiBimRoomPlanning_.status = DdgiBimRoomPlanning::Status::Rejected;
    ddgiBimRoomPlanning_.diagnostic = error.what();
    return;
  }
  if (event.expectFailure)
    throw std::runtime_error("The capture BIM room plan operation unexpectedly succeeded");
}

void RendererFrontend::appendDdgiBimRoomPlanTelemetry(nlohmann::json& json) const {
  using Status = DdgiBimRoomPlanning::Status;
  const auto& planner = ddgiBimRoomPlanning_;
  const auto status = [state = planner.status] {
    switch (state) {
    case Status::Idle: return "idle";
    case Status::Planning: return "planning";
    case Status::Preview: return "preview";
    case Status::Applied: return "applied";
    case Status::Rejected: return "rejected";
    }
    return "idle";
  }();
  auto& row = json["roomPlan"];
  row = {{"state", status}, {"diagnostic", planner.diagnostic},
         {"rooms", nlohmann::json::array()}, {"volumes", nlohmann::json::array()}};
  if (!planner.result) return;
  const auto& result = *planner.result;
  const auto& plan = result.plan;
  row["sourceUnits"] = result.sourceUnits;
  if (result.metersPerSourceUnit) row["metersPerSourceUnit"] = *result.metersPerSourceUnit;
  if (result.effectiveImportScale) row["effectiveImportScale"] = *result.effectiveImportScale;
  row["baseProbeCount"] = plan.baseProbeCount;
  row["totalProbeCount"] = plan.totalProbeCount;
  row["basePayloadBytes"] = plan.basePayloadBytes;
  row["payloadBytes"] = plan.payloadBytes;
  row["framesPerProbeCycle"] = plan.framesPerProbeCycle;
  for (const auto& room : plan.rooms) {
    row["rooms"].push_back({{"identity", room.identity}, {"label", room.label}, {"storey", room.storey},
        {"status", ddgiBimRoomCoverageName(room.status)}, {"reason", room.reason},
        {"volumeIndex", room.volumeIndex ? nlohmann::json(*room.volumeIndex) : nlohmann::json(nullptr)},
        {"boundsMin", room.bounds.valid ? vectorJson(room.bounds.minimum) : nlohmann::json(nullptr)},
        {"boundsMax", room.bounds.valid ? vectorJson(room.bounds.maximum) : nlohmann::json(nullptr)}});
  }
  for (const auto& volume : plan.volumes) {
    const auto& desc = plan.settings.additionalVolumes.at(volume.volumeIndex - 1);
    auto rooms = nlohmann::json::array();
    for (const auto index : volume.roomIndices) rooms.push_back(plan.rooms.at(index).identity);
    row["volumes"].push_back({{"volumeIndex", volume.volumeIndex}, {"probeCount", volume.probeCount},
        {"origin", vectorJson(desc.origin)}, {"spacing", vectorJson(desc.spacing)},
        {"counts", vectorJson(desc.counts)}, {"blendWidth", vectorJson(desc.spacing * plan.settings.volumeBlendCells)},
        {"rooms", std::move(rooms)}});
  }
}

} // namespace container::renderer
