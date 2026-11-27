#include "Container/renderer/core/RendererFrontend.h"

#include "Container/app/AppConfig.h"
#include "Container/renderer/bim/BimManager.h"
#include "Container/utility/GuiManager.h"
#include "Container/utility/Logger.h"

#include <imgui.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <span>
#include <stdexcept>

namespace container::renderer {
namespace {

constexpr ImU32 plannedColor = IM_COL32(255, 183, 64, 255);
constexpr ImU32 observedColor = IM_COL32(32, 231, 211, 255);
constexpr ImU32 robotColor = IM_COL32(165, 243, 91, 255);
constexpr ImU32 assetColor = IM_COL32(239, 114, 249, 255);
constexpr ImU32 markerBorder = IM_COL32(19, 25, 33, 255);
constexpr double nanosecondsPerSecond = 1'000'000'000.0;
constexpr size_t maxPathSegments = 4096;
constexpr size_t maxMarkers = 256;

struct TrackStyle {
  const char* kind;
  const char* label;
};

TrackStyle trackStyle(const robotics::TwinPayload& payload) {
  if (std::holds_alternative<robotics::TwinRobotPose>(payload)) return {"robot-pose", "Robot pose"};
  if (std::holds_alternative<robotics::TwinSensorStatus>(payload)) return {"sensor-status", "Sensor"};
  if (std::holds_alternative<robotics::TwinAssetState>(payload)) return {"bim-asset-state", "BIM asset"};
  if (std::holds_alternative<robotics::TwinRobotHealth>(payload)) return {"robot-health", "Robot health"};
  if (std::holds_alternative<robotics::TwinTaskStatus>(payload)) return {"task-status", "Task"};
  return {"planned-path", "Planned path"};
}

bool finite(glm::dvec3 point) {
  return std::isfinite(point.x) && std::isfinite(point.y) && std::isfinite(point.z);
}

std::optional<ImVec2> projectPoint(const glm::dmat4& viewProjection, const ImGuiViewport& viewport,
                                   glm::dvec3 position) {
  const glm::dvec4 clip = viewProjection * glm::dvec4(position, 1.0);
  if (!std::isfinite(clip.w) || clip.w <= 0.0)
    return std::nullopt;
  const auto ndc = glm::dvec3(clip) / clip.w;
  if (!finite(ndc) || ndc.z < 0.0 || ndc.z > 1.0)
    return std::nullopt;
  // The renderer uses a negative-height viewport. ImGui uses top-left
  // logical pixels, so invert NDC Y and scale to the GUI viewport directly.
  return ImVec2{
      viewport.Pos.x + static_cast<float>(0.5 * std::clamp(ndc.x, -32.0, 32.0) + 0.5) * viewport.Size.x,
      viewport.Pos.y + static_cast<float>(0.5 - 0.5 * std::clamp(ndc.y, -32.0, 32.0)) * viewport.Size.y,
  };
}

void drawTrackState(const robotics::TwinTrackSample& track, bool assetResolved) {
  if (const auto* sensor = std::get_if<robotics::TwinSensorStatus>(&track.event.payload)) {
    ImGui::TextUnformatted(sensor->state.c_str());
  } else if (const auto* asset = std::get_if<robotics::TwinAssetState>(&track.event.payload)) {
    ImGui::TextUnformatted(assetResolved ? asset->state.c_str() : "Unresolved BIM asset");
    if (!assetResolved && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", asset->state.c_str());
  } else if (const auto* robot = std::get_if<robotics::TwinRobotPose>(&track.event.payload)) {
    ImGui::Text("%.2f, %.2f, %.2f m", robot->position.x, robot->position.y, robot->position.z);
    if (track.distanceToPlanMeters && ImGui::IsItemHovered())
      ImGui::SetTooltip("%.2f m from plan", *track.distanceToPlanMeters);
  } else if (const auto* plan = std::get_if<robotics::TwinPlannedPath>(&track.event.payload)) {
    ImGui::Text("%zu waypoints", plan->points.size());
  } else if (const auto* health = std::get_if<robotics::TwinRobotHealth>(&track.event.payload)) {
    if (health->batteryPercent)
      ImGui::Text("%s (%.0f%% battery)", health->state.c_str(), *health->batteryPercent);
    else
      ImGui::TextUnformatted(health->state.c_str());
  } else if (const auto* task = std::get_if<robotics::TwinTaskStatus>(&track.event.payload)) {
    if (task->progress)
      ImGui::Text("%s (%.0f%%)", task->state.c_str(), 100.0 * *task->progress);
    else
      ImGui::TextUnformatted(task->state.c_str());
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Task: %s", task->taskId.c_str());
  }
}

} // namespace

void RendererFrontend::initializeBimTwinReplay() {
  const auto replay = svc_.config.bimTwinReplay;
  if (!replay)
    return;
  if (!subs_.bimManager)
    throw std::runtime_error("BIM twin replay: no active BIM model");
  bimTwin_.emplace();
  refreshBimTwinBinding();
  if (!bimTwin_->sourceError.empty())
    throw std::runtime_error("BIM twin replay: " + bimTwin_->sourceError);
  const auto requested = svc_.config.bimTwinStartNanoseconds.value_or(replay->firstTimestamp());
  const auto time = std::clamp(requested, replay->firstTimestamp(), replay->lastTimestamp());
  bimTwin_->sample = replay->sample(time);
}

void RendererFrontend::refreshBimTwinBinding() {
  if (!bimTwin_)
    return;
  auto& state = *bimTwin_;
  const auto* model = subs_.bimManager.get();
  const auto revision = model ? model->metadataRevision() : 0;
  if (state.checkedModel == model && state.checkedRevision == revision)
    return;
  state.checkedModel = model;
  state.checkedRevision = revision;
  state.playing = false;
  state.assetObjects.clear();
  state.sourceError.clear();

  const auto& recording = svc_.config.bimTwinReplay->recording();
  const auto* labels = model && model->hasScene() ? model->sensorLabels() : nullptr;
  if (!labels || !labels->sceneSource || !labels->metersPerRendererUnit) {
    state.sourceError = "the active model has no BIM source and metre evidence";
  } else if (labels->sourceNamespace != recording.sourceNamespace) {
    state.sourceError = "the active BIM namespace differs from the recording";
  } else if (labels->sceneSource->layers != recording.sceneSource.layers) {
    state.sourceError = "the ordered BIM source fingerprints differ from the recording";
  } else {
    const auto actual = labels->sceneSource->metersPerUnscaledRendererUnit;
    const auto expected = recording.sceneSource.metersPerUnscaledRendererUnit;
    // Imported render coordinates can have float-scale rounding. Compare the
    // unscaled physical units with the dataset recorder's relative tolerance.
    if (std::abs(actual - expected) > 1e-6 * std::max(actual, expected))
      state.sourceError = "the active BIM metre scale differs from the recording";
  }
  const auto logger = container::log::ContainerLogger::instance().renderer();
  if (!state.sourceError.empty()) {
    logger->warn("BIM twin replay detached: {}", state.sourceError);
    return;
  }

  for (const auto& event : recording.events)
    if (std::holds_alternative<robotics::TwinAssetState>(event.payload))
      state.assetObjects.try_emplace(event.entityId);
  if (state.assetObjects.empty())
    return;

  std::map<uint32_t, std::vector<uint32_t>*> instances;
  for (const auto& instance : labels->instances) {
    const auto entry = state.assetObjects.find(instance.entityId);
    if (entry != state.assetObjects.end())
      instances.emplace(instance.instanceId, &entry->second);
  }
  for (const auto& element : model->elementMetadata()) {
    if (element.sourceElementIndex >= labels->elements.size())
      continue;
    const auto instance = instances.find(labels->elements[element.sourceElementIndex].instanceId);
    if (instance != instances.end())
      instance->second->push_back(element.objectIndex);
  }
  size_t missing = 0;
  constexpr size_t individualDiagnostics = 8;
  for (const auto& [entity, objects] : state.assetObjects) {
    if (!objects.empty())
      continue;
    if (++missing <= individualDiagnostics)
      logger->warn("BIM twin replay: unresolved BIM asset '{}'", entity);
  }
  if (missing > individualDiagnostics)
    logger->warn("BIM twin replay: {} additional unresolved BIM assets", missing - individualDiagnostics);
}

void RendererFrontend::advanceBimTwinReplay(float seconds) {
  if (!bimTwin_ || !bimTwin_->playing || !std::isfinite(seconds) || seconds <= 0.0f)
    return;
  auto& state = *bimTwin_;
  const auto& replay = *svc_.config.bimTwinReplay;
  const auto step = static_cast<uint64_t>(std::min(double(seconds), 0.25) * nanosecondsPerSecond);
  const auto current = state.sample.timestampNanoseconds;
  const auto next = current + std::min(step, replay.lastTimestamp() - current);
  if (next != current)
    state.sample = replay.sample(next);
  if (next == replay.lastTimestamp())
    state.playing = false;
}

std::optional<glm::dvec3> RendererFrontend::bimTwinRendererPosition(glm::dvec3 meters) const {
  const auto& model = *subs_.bimManager;
  const auto scale = *model.sensorLabels()->metersPerRendererUnit;
  // Inverse of the building-local REP-103 transform in coordinate-conventions.md.
  const auto position = glm::dvec3(-meters.y, meters.z, -meters.x) / scale + glm::dvec3(model.rootTranslation());
  return finite(position) ? std::optional{position} : std::nullopt;
}

std::optional<glm::dvec3> RendererFrontend::bimTwinAssetPosition(std::string_view entity) const {
  const auto found = bimTwin_->assetObjects.find(entity);
  if (found == bimTwin_->assetObjects.end())
    return std::nullopt;
  bool valid = false;
  glm::dvec3 minimum(0.0), maximum(0.0);
  for (const auto object : found->second) {
    const auto metadata = subs_.bimManager->metadataForObject(object);
    if (!metadata || !metadata->bounds.valid)
      continue;
    minimum = valid ? glm::min(minimum, glm::dvec3(metadata->bounds.min)) : glm::dvec3(metadata->bounds.min);
    maximum = valid ? glm::max(maximum, glm::dvec3(metadata->bounds.max)) : glm::dvec3(metadata->bounds.max);
    valid = true;
  }
  if (!valid)
    return std::nullopt;
  return (minimum + maximum) * 0.5;
}

void RendererFrontend::drawBimTwinReplay() {
  if (!bimTwin_)
    return;
  auto& state = *bimTwin_;
  const auto& replay = *svc_.config.bimTwinReplay;
  const auto* viewport = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + 12.0f,
                                viewport->WorkPos.y + viewport->WorkSize.y - 12.0f),
                          ImGuiCond_FirstUseEver, ImVec2(0, 1));
  ImGui::SetNextWindowSize(ImVec2(std::min(460.0f, viewport->WorkSize.x * 0.49f),
                                  std::min(280.0f, viewport->WorkSize.y * 0.43f)),
                           ImGuiCond_FirstUseEver);
  if (ImGui::Begin("BIM Twin Replay")) {
    if (!state.sourceError.empty()) {
      ImGui::TextWrapped("Replay paused. %s.", state.sourceError.c_str());
    } else {
      const auto first = replay.firstTimestamp();
      const auto last = replay.lastTimestamp();
      ImGui::BeginDisabled(first == last);
      if (ImGui::Button(state.playing ? "Pause" : "Play")) {
        if (!state.playing && state.sample.timestampNanoseconds == last)
          state.sample = replay.sample(first);
        state.playing = !state.playing;
      }
      ImGui::SameLine();
      if (ImGui::Button("Reset")) {
        state.sample = replay.sample(first);
        state.playing = false;
      }
      const double zero = 0.0;
      const double duration = double(last - first) / nanosecondsPerSecond;
      double elapsed = double(state.sample.timestampNanoseconds - first) / nanosecondsPerSecond;
      ImGui::SetNextItemWidth(-1.0f);
      if (ImGui::SliderScalar("##TwinTime", ImGuiDataType_Double, &elapsed, &zero, &duration,
                              "%.3f s", ImGuiSliderFlags_NoInput)) {
        const auto offset = elapsed * nanosecondsPerSecond;
        const auto selected = offset >= double(last - first) ? last : first + static_cast<uint64_t>(offset);
        state.sample = replay.sample(selected);
        state.playing = false;
      }
      ImGui::EndDisabled();
      ImGui::Text("Clock: %s", replay.recording().clockId.c_str());
      ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(plannedColor), "Planned");
      ImGui::SameLine();
      ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(observedColor), "Observed");
      ImGui::SameLine();
      ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(assetColor), "BIM asset");
      if (state.sample.tracks.empty())
        ImGui::TextDisabled("No observations at this time");
      const ImVec2 tableSize(0.0f, std::max(55.0f, ImGui::GetContentRegionAvail().y));
      if (ImGui::BeginTable("TwinTracks", 3,
                           ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY,
                           tableSize)) {
        ImGui::TableSetupColumn("Track");
        ImGui::TableSetupColumn("State");
        ImGui::TableSetupColumn("Age");
        ImGui::TableHeadersRow();
        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(state.sample.tracks.size()));
        while (clipper.Step()) {
          for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
            const auto& track = state.sample.tracks[static_cast<size_t>(row)];
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextUnformatted(trackStyle(track.event.payload).label);
            if (ImGui::IsItemHovered())
              ImGui::SetTooltip("%s\nSource: %s", track.event.entityId.c_str(), track.event.source.c_str());
            ImGui::TableSetColumnIndex(1);
            drawTrackState(track, !std::holds_alternative<robotics::TwinAssetState>(track.event.payload) ||
                                    bimTwinAssetPosition(track.event.entityId).has_value());
            ImGui::TableSetColumnIndex(2);
            const auto age = state.sample.timestampNanoseconds - track.event.timestampNanoseconds;
            ImGui::Text("%.3f s%s", double(age) / nanosecondsPerSecond, track.stale ? " (stale)" : "");
          }
        }
        ImGui::EndTable();
      }
    }
  }
  ImGui::End();

  if (!state.sourceError.empty() || !subs_.guiManager->editorOverlaysEnabled())
    return;
  refreshCameraData();
  const glm::dmat4 viewProjection(buffers_.cameraData.unjitteredViewProj);
  auto* overlay = ImGui::GetBackgroundDrawList();
  size_t remainingSegments = maxPathSegments / 2;
  size_t remainingMarkers = maxMarkers;
  const auto project = [&](glm::dvec3 meters) -> std::optional<ImVec2> {
    const auto world = bimTwinRendererPosition(meters);
    return world ? projectPoint(viewProjection, *viewport, *world) : std::nullopt;
  };
  const auto drawPath = [&](std::span<const glm::dvec3> path, ImU32 color, float width) {
    if (path.size() < 2 || remainingSegments == 0)
      return;
    // Bound ImGui geometry for long trails while keeping both endpoints.
    const size_t last = path.size() - 1;
    const size_t steps = std::min({last, size_t{256}, remainingSegments});
    const size_t stride = (last + steps - 1) / steps;
    auto previous = project(path.front());
    for (size_t index = stride;; index += stride) {
      --remainingSegments;
      const auto current = project(path[std::min(index, last)]);
      if (previous && current)
        overlay->AddLine(*previous, *current, color, width);
      if (index >= last)
        break;
      previous = current;
    }
  };
  for (const auto& track : state.sample.tracks)
    if (const auto* plan = std::get_if<robotics::TwinPlannedPath>(&track.event.payload))
      drawPath(plan->points, plannedColor, 3.5f);

  remainingSegments += maxPathSegments / 2;
  for (const auto& track : state.sample.tracks) {
    if (const auto* robot = std::get_if<robotics::TwinRobotPose>(&track.event.payload)) {
      drawPath(track.observedPath, observedColor, 2.5f);
      if (remainingMarkers == 0)
        continue;
      --remainingMarkers;
      if (const auto center = project(robot->position)) {
        const auto ahead = robot->position + 0.40 * glm::dvec3(std::cos(robot->yaw), std::sin(robot->yaw), 0.0);
        if (const auto tip = project(ahead)) {
          overlay->AddLine(*center, *tip, markerBorder, 5.0f);
          overlay->AddLine(*center, *tip, robotColor, 3.0f);
        }
        overlay->AddCircleFilled(*center, 7.0f, markerBorder);
        overlay->AddCircleFilled(*center, 5.0f, robotColor);
      }
    } else if (std::holds_alternative<robotics::TwinAssetState>(track.event.payload)) {
      if (remainingMarkers == 0)
        continue;
      --remainingMarkers;
      const auto position = bimTwinAssetPosition(track.event.entityId);
      if (!position)
        continue;
      if (const auto center = projectPoint(viewProjection, *viewport, *position)) {
        const ImVec2 diamond[]{{center->x, center->y - 8}, {center->x + 8, center->y},
                               {center->x, center->y + 8}, {center->x - 8, center->y}};
        overlay->AddQuadFilled(diamond[0], diamond[1], diamond[2], diamond[3], assetColor);
        overlay->AddQuad(diamond[0], diamond[1], diamond[2], diamond[3], markerBorder, 2.0f);
      }
    }
  }
}

nlohmann::json RendererFrontend::bimTwinCaptureTelemetry() const {
  using Json = nlohmann::json;
  const auto xyz = [](const std::optional<glm::dvec3>& point) {
    return point ? Json::array({point->x, point->y, point->z}) : Json(nullptr);
  };
  const auto& state = *bimTwin_;
  const auto& recording = svc_.config.bimTwinReplay->recording();
  const bool matched = state.sourceError.empty();
  Json tracks = Json::array();
  size_t resolved = 0, unresolved = 0;
  if (matched) {
    for (const auto& track : state.sample.tracks) {
      Json row{{"kind", trackStyle(track.event.payload).kind}, {"entityId", track.event.entityId},
               {"eventTimestampNanoseconds", track.event.timestampNanoseconds},
               {"source", track.event.source}, {"stale", track.stale}};
      if (const auto* robot = std::get_if<robotics::TwinRobotPose>(&track.event.payload)) {
        row["positionMeters"] = xyz(robot->position);
        row["rendererPosition"] = xyz(bimTwinRendererPosition(robot->position));
        row["yaw"] = robot->yaw;
        row["observedPointCount"] = track.observedPath.size();
        row["distanceToPlanMeters"] = track.distanceToPlanMeters ? Json(*track.distanceToPlanMeters) : Json(nullptr);
      } else if (const auto* asset = std::get_if<robotics::TwinAssetState>(&track.event.payload)) {
        const auto position = bimTwinAssetPosition(track.event.entityId);
        row["state"] = asset->state;
        row["rendererPosition"] = xyz(position);
        if (position) ++resolved;
        else ++unresolved;
      } else if (const auto* sensor = std::get_if<robotics::TwinSensorStatus>(&track.event.payload)) {
        row["state"] = sensor->state;
      } else if (const auto* plan = std::get_if<robotics::TwinPlannedPath>(&track.event.payload)) {
        row["pointCount"] = plan->points.size();
      } else if (const auto* health = std::get_if<robotics::TwinRobotHealth>(&track.event.payload)) {
        row["state"] = health->state;
        row["batteryPercent"] = health->batteryPercent ? Json(*health->batteryPercent) : Json(nullptr);
      } else if (const auto* task = std::get_if<robotics::TwinTaskStatus>(&track.event.payload)) {
        row["taskId"] = task->taskId;
        row["state"] = task->state;
        row["progress"] = task->progress ? Json(*task->progress) : Json(nullptr);
      }
      tracks.push_back(std::move(row));
    }
  }
  return {{"sourceMatched", matched}, {"sourceError", state.sourceError},
          {"sourceNamespace", recording.sourceNamespace}, {"clockId", recording.clockId},
          {"timestampNanoseconds", state.sample.timestampNanoseconds}, {"playing", state.playing},
          {"resolvedAssetCount", resolved}, {"unresolvedAssetCount", unresolved}, {"tracks", std::move(tracks)}};
}

} // namespace container::renderer
