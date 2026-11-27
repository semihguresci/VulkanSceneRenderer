#include "Container/renderer/core/RendererFrontend.h"

#include "Container/app/AppConfig.h"
#include "Container/renderer/bim/BimManager.h"
#include "Container/utility/GuiManager.h"
#include "Container/utility/Logger.h"

#include <imgui.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <span>
#include <stdexcept>

namespace container::renderer {
namespace {

constexpr ImU32 plannedColor = IM_COL32(255, 183, 64, 255);
constexpr ImU32 observedColor = IM_COL32(32, 231, 211, 255);
constexpr ImU32 robotColor = IM_COL32(165, 243, 91, 255);
constexpr ImU32 assetColor = IM_COL32(239, 114, 249, 255);
constexpr ImU32 warningColor = IM_COL32(255, 133, 78, 255);
constexpr ImU32 criticalColor = IM_COL32(255, 76, 103, 255);
constexpr ImU32 infoColor = IM_COL32(126, 226, 243, 255);
constexpr ImU32 frustumColor = IM_COL32(108, 160, 255, 255);
constexpr ImU32 occupancyColor = IM_COL32(179, 151, 255, 255);
constexpr ImU32 occupancyFill = IM_COL32(179, 151, 255, 46);
constexpr ImU32 markerBorder = IM_COL32(19, 25, 33, 255);
constexpr double nanosecondsPerSecond = 1'000'000'000.0;
constexpr size_t maxPathSegments = 4096;
constexpr size_t maxMarkers = 256;
constexpr size_t maxAlarmMarkers = 128;
constexpr size_t maxFrusta = 128;
constexpr size_t maxOccupiedRuns = 1024;
constexpr size_t maxRunProjections = maxOccupiedRuns * 8;

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
  if (std::holds_alternative<robotics::TwinOperatorAlarm>(payload)) return {"operator-alarm", "Alarm"};
  if (std::holds_alternative<robotics::TwinSensorFrustum>(payload)) return {"sensor-frustum", "Camera frustum"};
  if (std::holds_alternative<robotics::TwinOccupancyGrid>(payload)) return {"occupancy-grid", "Occupancy"};
  return {"planned-path", "Planned path"};
}

bool finite(glm::dvec3 point) {
  return std::isfinite(point.x) && std::isfinite(point.y) && std::isfinite(point.z);
}

ImVec2 screenPoint(const ImGuiViewport& viewport, glm::dvec3 ndc) {
  // Vulkan flips Y in the viewport. ImGui coordinates start at the top left.
  return {viewport.Pos.x + static_cast<float>(0.5 * std::clamp(ndc.x, -32.0, 32.0) + 0.5) * viewport.Size.x,
          viewport.Pos.y + static_cast<float>(0.5 - 0.5 * std::clamp(ndc.y, -32.0, 32.0)) * viewport.Size.y};
}

std::optional<ImVec2> projectPoint(const glm::dmat4& viewProjection, const ImGuiViewport& viewport,
                                   glm::dvec3 position) {
  const glm::dvec4 clip = viewProjection * glm::dvec4(position, 1.0);
  if (!std::isfinite(clip.w) || clip.w <= 0.0)
    return std::nullopt;
  const auto ndc = glm::dvec3(clip) / clip.w;
  if (!finite(ndc) || ndc.z < 0.0 || ndc.z > 1.0)
    return std::nullopt;
  return screenPoint(viewport, ndc);
}

std::array<double, 6> clipDistances(glm::dvec4 p) {
  return {p.w + p.x, p.w - p.x, p.w + p.y, p.w - p.y, p.z, p.w - p.z};
}

std::optional<std::array<ImVec2, 2>> projectSegment(const ImGuiViewport& viewport, glm::dvec4 a, glm::dvec4 b) {
  // Clip to Vulkan's homogeneous frustum before the perspective divide.
  // Reverse-Z still places both depth boundaries at 0 <= z <= w.
  const auto start = clipDistances(a);
  const auto end = clipDistances(b);
  double first = 0.0, last = 1.0;
  for (size_t i = 0; i < start.size(); ++i) {
    if (!std::isfinite(start[i]) || !std::isfinite(end[i]) || (start[i] < 0.0 && end[i] < 0.0))
      return std::nullopt;
    if (start[i] < 0.0)
      first = std::max(first, start[i] / (start[i] - end[i]));
    else if (end[i] < 0.0)
      last = std::min(last, start[i] / (start[i] - end[i]));
  }
  const glm::dvec4 p = glm::mix(a, b, first), q = glm::mix(a, b, last);
  if (first > last || p.w <= 0.0 || q.w <= 0.0)
    return std::nullopt;
  return std::array{screenPoint(viewport, glm::dvec3(p) / p.w), screenPoint(viewport, glm::dvec3(q) / q.w)};
}

ImU32 alarmColor(const robotics::TwinOperatorAlarm& alarm) {
  if (alarm.severity == "critical") return criticalColor;
  if (alarm.severity == "warning") return warningColor;
  return infoColor;
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
  } else if (const auto* alarm = std::get_if<robotics::TwinOperatorAlarm>(&track.event.payload)) {
    if (alarm->active && !track.stale)
      ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(alarmColor(*alarm)), "%s", alarm->message.c_str());
    else
      ImGui::TextDisabled("%s", alarm->active ? alarm->message.c_str() : "Cleared");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\n%s", alarm->severity.c_str(), alarm->message.c_str());
  } else if (const auto* camera = std::get_if<robotics::TwinSensorFrustum>(&track.event.payload)) {
    ImGui::Text("%u x %u, %.2f m range", camera->width, camera->height, camera->farMeters);
  } else if (const auto* grid = std::get_if<robotics::TwinOccupancyGrid>(&track.event.payload)) {
    ImGui::Text("%u x %u, %.2f m cells", grid->width, grid->height, grid->resolutionMeters);
  }
}

} // namespace

void RendererFrontend::initializeBimTwinReplay() {
  const auto replay = svc_.config.bimTwinReplay;
  if (!replay)
    return;
  if (!subs_.bimManager)
    throw std::runtime_error("BIM twin replay: no active BIM model");
  auto& state = bimTwin_.emplace();
  state.replay = replay;
  state.followLatest = !svc_.config.bimTwinFollowPath.empty() && !svc_.config.bimTwinStartNanoseconds;
  refreshBimTwinBinding();
  if (!state.sourceError.empty())
    throw std::runtime_error("BIM twin replay: " + state.sourceError);
  const auto requested = svc_.config.bimTwinStartNanoseconds.value_or(
      state.followLatest ? replay->lastTimestamp() : replay->firstTimestamp());
  const auto time = std::clamp(requested, replay->firstTimestamp(), replay->lastTimestamp());
  state.sample = replay->sample(time);
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

  const auto& recording = state.replay->recording();
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

void RendererFrontend::updateBimTwinFollow() {
  auto& state = *bimTwin_;
  if (state.followJob.valid()) {
    if (state.followJob.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
      return;
    if (auto update = state.followJob.get()) {
      state.watchedVersion = update->version;
      if (update->snapshot) {
        const auto& next = update->snapshot->recording();
        const auto& current = state.replay->recording();
        if (next.clockId != current.clockId || next.sourceNamespace != current.sourceNamespace ||
            next.sceneSource.layers != current.sceneSource.layers ||
            next.sceneSource.metersPerUnscaledRendererUnit != current.sceneSource.metersPerUnscaledRendererUnit) {
          update->error = "Snapshot uses a different BIM source, scale or clock";
        } else if (update->snapshot->lastTimestamp() < state.replay->lastTimestamp()) {
          update->error = "Snapshot ends before the latest accepted telemetry";
        } else {
          auto nextReplay = std::move(update->snapshot);
          const auto time = state.followLatest ? nextReplay->lastTimestamp()
              : std::clamp(state.sample.timestampNanoseconds, nextReplay->firstTimestamp(), nextReplay->lastTimestamp());
          state.sample = nextReplay->sample(time);
          state.replay = std::move(nextReplay);
          state.checkedModel = nullptr;
          const bool wasPlaying = state.playing;
          refreshBimTwinBinding();
          state.playing = wasPlaying && state.sourceError.empty();
        }
      }
      if (!update->error.empty() && update->error != state.followError)
        container::log::ContainerLogger::instance().renderer()->warn("BIM twin follow: {}", update->error);
      state.followError = std::move(update->error);
    }
  }

  const auto now = std::chrono::steady_clock::now();
  if (now < state.nextFollowPoll)
    return;
  state.nextFollowPoll = now + std::chrono::milliseconds(500);
  state.followJob = std::async(std::launch::async,
      [path = svc_.config.bimTwinFollowPath, seen = state.watchedVersion]() -> std::optional<BimTwinReplayState::FollowUpdate> {
        const auto version = [&] {
          return BimTwinReplayState::FileVersion{std::filesystem::last_write_time(path), std::filesystem::file_size(path)};
        };
        BimTwinReplayState::FollowUpdate update;
        try {
          update.version = version();
          if (update.version == seen)
            return std::nullopt;
          update.snapshot = std::make_shared<const robotics::TwinReplay>(robotics::loadTwinRecording(path));
          if (*update.version != version())
            return std::nullopt;
        } catch (const std::exception& error) {
          update.snapshot.reset();
          update.error = error.what();
        }
        return update;
      });
}

void RendererFrontend::advanceBimTwinReplay(float seconds) {
  if (!bimTwin_)
    return;
  if (!svc_.config.bimTwinFollowPath.empty())
    updateBimTwinFollow();
  if (!bimTwin_->playing || !std::isfinite(seconds) || seconds <= 0.0f)
    return;
  auto& state = *bimTwin_;
  const auto& replay = *state.replay;
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
  const auto& replay = *state.replay;
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
      if (!svc_.config.bimTwinFollowPath.empty()) {
        if (ImGui::Checkbox("Follow latest", &state.followLatest) && state.followLatest) {
          state.playing = false;
          state.sample = replay.sample(last);
        }
        if (!state.followError.empty())
          ImGui::TextWrapped("Keeping the previous snapshot. %s", state.followError.c_str());
      }
      ImGui::BeginDisabled(first == last || state.followLatest);
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
      if (std::ranges::any_of(state.sample.tracks, [](const auto& track) {
            return std::holds_alternative<robotics::TwinSensorFrustum>(track.event.payload);
          })) {
        ImGui::SameLine();
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(frustumColor), "Camera");
      }
      if (std::ranges::any_of(state.sample.tracks, [](const auto& track) {
            return std::holds_alternative<robotics::TwinOccupancyGrid>(track.event.payload);
          })) {
        ImGui::SameLine();
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(occupancyColor), "Occupied");
      }
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
            if (const auto* task = std::get_if<robotics::TwinTaskStatus>(&track.event.payload))
              ImGui::Text("Task / %s", task->taskId.c_str());
            else if (const auto* alarm = std::get_if<robotics::TwinOperatorAlarm>(&track.event.payload))
              ImGui::Text("Alarm / %s", alarm->alarmId.c_str());
            else
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
  const auto drawSegment = [&](glm::dvec3 a, glm::dvec3 b, ImU32 color, float width) {
    const auto start = bimTwinRendererPosition(a), end = bimTwinRendererPosition(b);
    if (start && end) {
      const auto line = projectSegment(*viewport, viewProjection * glm::dvec4(*start, 1.0),
                                       viewProjection * glm::dvec4(*end, 1.0));
      if (line) {
        overlay->AddLine((*line)[0], (*line)[1], color, width);
        return true;
      }
    }
    return false;
  };
  const auto mayBeVisible = [&](std::span<const glm::dvec3> corners) {
    std::array<bool, 6> excluded;
    excluded.fill(true);
    for (const auto& corner : corners) {
      const auto world = bimTwinRendererPosition(corner);
      if (!world)
        return false;
      const auto distances = clipDistances(viewProjection * glm::dvec4(*world, 1.0));
      for (size_t plane = 0; plane < excluded.size(); ++plane)
        excluded[plane] = excluded[plane] && distances[plane] < 0.0;
    }
    return std::ranges::none_of(excluded, [](bool outside) { return outside; });
  };
  size_t remainingRuns = maxOccupiedRuns, remainingProjections = maxRunProjections, remainingFrusta = maxFrusta;
  for (const auto& track : state.sample.tracks) {
    if (track.stale)
      continue;
    if (const auto* grid = std::get_if<robotics::TwinOccupancyGrid>(&track.event.payload)) {
      if (remainingRuns == 0 || remainingProjections == 0)
        continue;
      const glm::dvec3 column = grid->resolutionMeters * glm::dvec3(std::cos(grid->yaw), std::sin(grid->yaw), 0.0);
      const glm::dvec3 row{-column.y, column.x, 0.0};
      const auto u = double(grid->width) * column, v = double(grid->height) * row;
      const std::array bounds{grid->origin, grid->origin + u, grid->origin + u + v, grid->origin + v};
      if (!mayBeVisible(bounds))
        continue;
      for (uint32_t y = 0; y < grid->height && remainingRuns > 0 && remainingProjections > 0; ++y) {
        const auto cells = std::string_view(grid->cells).substr(size_t(y) * grid->width, grid->width);
        for (size_t x = cells.find('#'); x != cells.npos && remainingRuns > 0 && remainingProjections > 0;) {
          auto end = cells.find_first_not_of('#', x);
          if (end == cells.npos) end = cells.size();
          const auto a = grid->origin + double(x) * column + double(y) * row;
          const auto b = a + double(end - x) * column;
          const std::array corners{a, b, b + row, a + row};
          if (mayBeVisible(corners)) {
            const auto p = project(corners[0]), q = project(corners[1]);
            const auto r = project(corners[2]), s = project(corners[3]);
            if (p && q && r && s)
              overlay->AddQuadFilled(*p, *q, *r, *s, occupancyFill);
            for (size_t i = 0; i < corners.size(); ++i)
              drawSegment(corners[i], corners[(i + 1) % corners.size()], occupancyColor, 2.5f);
            --remainingRuns;
          }
          --remainingProjections;
          x = cells.find('#', end);
        }
      }
    } else if (const auto* camera = std::get_if<robotics::TwinSensorFrustum>(&track.event.payload);
               camera && remainingFrusta > 0) {
      const auto corners = robotics::twinFrustumCorners(*camera);
      if (!mayBeVisible(corners))
        continue;
      bool visible = false;
      for (size_t i = 0; i < 4; ++i) {
        const size_t next = (i + 1) % 4;
        visible = drawSegment(corners[i], corners[next], frustumColor, 2.75f) || visible;
        visible = drawSegment(corners[i + 4], corners[next + 4], frustumColor, 2.75f) || visible;
        visible = drawSegment(corners[i], corners[i + 4], frustumColor, 2.75f) || visible;
      }
      if (visible)
        --remainingFrusta;
    }
  }
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
  size_t remainingAlarms = maxAlarmMarkers;
  for (const auto& track : state.sample.tracks) {
    if (remainingAlarms == 0)
      break;
    const auto* alarm = std::get_if<robotics::TwinOperatorAlarm>(&track.event.payload);
    if (!alarm || !alarm->active || track.stale)
      continue;
    if (const auto center = project(alarm->position)) {
      const ImVec2 top{center->x, center->y - 13.0f};
      const ImVec2 left{center->x - 12.0f, center->y + 9.0f};
      const ImVec2 right{center->x + 12.0f, center->y + 9.0f};
      overlay->AddTriangleFilled(top, right, left, alarmColor(*alarm));
      overlay->AddTriangle(top, right, left, markerBorder, 2.0f);
      overlay->AddLine({center->x, center->y - 5.0f}, {center->x, center->y + 1.0f}, markerBorder, 2.0f);
      overlay->AddCircleFilled({center->x, center->y + 5.0f}, 1.25f, markerBorder, 8);
      --remainingAlarms;
    }
  }
}

nlohmann::json RendererFrontend::bimTwinCaptureTelemetry() const {
  using Json = nlohmann::json;
  const auto xyz = [](const std::optional<glm::dvec3>& point) {
    return point ? Json::array({point->x, point->y, point->z}) : Json(nullptr);
  };
  const auto& state = *bimTwin_;
  const auto& recording = state.replay->recording();
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
      } else if (const auto* alarm = std::get_if<robotics::TwinOperatorAlarm>(&track.event.payload)) {
        row["alarmId"] = alarm->alarmId;
        row["severity"] = alarm->severity;
        row["message"] = alarm->message;
        row["active"] = alarm->active;
        row["position"] = xyz(alarm->position);
        row["rendererPosition"] = xyz(bimTwinRendererPosition(alarm->position));
      } else if (const auto* camera = std::get_if<robotics::TwinSensorFrustum>(&track.event.payload)) {
        Json corners = Json::array();
        for (const auto& point : robotics::twinFrustumCorners(*camera))
          corners.push_back(xyz(bimTwinRendererPosition(point)));
        row["rendererCorners"] = std::move(corners);
      } else if (const auto* grid = std::get_if<robotics::TwinOccupancyGrid>(&track.event.payload)) {
        row["width"] = grid->width;
        row["height"] = grid->height;
        row["yaw"] = grid->yaw;
        row["resolutionMeters"] = grid->resolutionMeters;
        row["origin"] = xyz(grid->origin);
        row["rendererOrigin"] = xyz(bimTwinRendererPosition(grid->origin));
        row["occupiedCells"] = std::ranges::count(grid->cells, '#');
      }
      tracks.push_back(std::move(row));
    }
  }
  Json result{{"sourceMatched", matched}, {"sourceError", state.sourceError},
              {"sourceNamespace", recording.sourceNamespace}, {"clockId", recording.clockId},
              {"timestampNanoseconds", state.sample.timestampNanoseconds}, {"playing", state.playing},
              {"resolvedAssetCount", resolved}, {"unresolvedAssetCount", unresolved}, {"tracks", std::move(tracks)}};
  if (!svc_.config.bimTwinFollowPath.empty()) {
    result["following"] = state.followLatest;
    result["followError"] = state.followError;
  }
  return result;
}

} // namespace container::renderer
