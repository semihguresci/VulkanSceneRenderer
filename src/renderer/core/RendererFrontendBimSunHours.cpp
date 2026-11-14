#include "Container/renderer/core/RendererFrontend.h"

#include "Container/renderer/bim/BimManager.h"
#include "Container/renderer/bim/BimSunHoursOverlay.h"
#include "Container/renderer/core/FrameRecorder.h"
#include "Container/renderer/temporal/TemporalCapture.h"
#include "Container/utility/AllocationManager.h"
#include "Container/utility/GuiManager.h"
#include "Container/utility/Platform.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <fstream>
#include <iomanip>
#include <locale>
#include <stdexcept>

namespace container::renderer {
namespace {

std::vector<BimSunTriangle> snapshotBimSunHours(const BimManager& model) {
  const auto indices = model.indices();
  const auto vertices = model.vertices();
  const auto& objects = model.objectData();
  const auto lists = {&model.opaqueDrawCommands(), &model.transparentDrawCommands()};
  size_t triangles = 0;
  for (const auto* draws : lists) for (const auto& draw : *draws) {
    const size_t first = draw.firstIndex, count = draw.indexCount;
    if (first > indices.size() || count > indices.size() - first || count % 3 != 0 ||
        draw.objectIndex > objects.size() || draw.instanceCount > objects.size() - draw.objectIndex)
      throw std::runtime_error("The BIM model has an invalid triangle range");
    const size_t perInstance = count / 3;
    if (draw.instanceCount && perInstance > (kMaxBimSunHoursTriangles - triangles) / draw.instanceCount)
      throw std::runtime_error("The loaded BIM model exceeds the sun study triangle limit");
    triangles += perInstance * draw.instanceCount;
  }
  std::vector<BimSunTriangle> snapshot;
  snapshot.reserve(triangles);
  for (const auto* draws : lists) for (const auto& draw : *draws) {
    for (uint32_t instance = 0; instance < draw.instanceCount; ++instance) {
      const glm::dmat4 transform(objects[draw.objectIndex + instance].model);
      const size_t end = size_t(draw.firstIndex) + draw.indexCount;
      for (size_t index = draw.firstIndex; index < end; index += 3) {
        auto& triangle = snapshot.emplace_back();
        for (size_t corner = 0; corner < 3; ++corner) {
          const auto vertex = indices[index + corner];
          if (vertex >= vertices.size()) throw std::runtime_error("A BIM triangle vertex is missing");
          triangle.vertices[corner] = glm::dvec3(transform * glm::dvec4(vertices[vertex].position, 1.0));
        }
      }
    }
  }
  return snapshot;
}

std::vector<BimSunHoursVertex> sunHoursVertices(const BimSunHoursResult& result) {
  const auto& s = result.settings;
  const double maxHours = (s.endMinute - s.startMinute) / 60.0;
  std::vector<BimSunHoursVertex> vertices;
  vertices.reserve(result.hours.size() * 6);
  for (uint32_t row = 0; row < s.rows; ++row) for (uint32_t column = 0; column < s.columns; ++column) {
    const auto color = BimSunHoursColor(result.hours[size_t(row) * s.columns + column], maxHours);
    const float left = float(std::lerp(s.minX, s.maxX, double(column) / s.columns));
    const float right = float(std::lerp(s.minX, s.maxX, double(column + 1) / s.columns));
    const float front = float(std::lerp(s.minZ, s.maxZ, double(row) / s.rows));
    const float back = float(std::lerp(s.minZ, s.maxZ, double(row + 1) / s.rows));
    const float y = float(s.elevation);
    const glm::vec3 a{left, y, front}, b{right, y, front}, c{left, y, back}, d{right, y, back};
    for (const auto position : {a, c, b, b, c, d}) vertices.push_back({position, color});
  }
  return vertices;
}

nlohmann::json sunStudySettings(const BimSunHoursSettings& s) {
  return {{"minX", s.minX}, {"maxX", s.maxX}, {"minZ", s.minZ}, {"maxZ", s.maxZ}, {"height", s.elevation},
          {"columns", s.columns}, {"rows", s.rows}, {"latitude", s.solar.latitudeDegrees},
          {"longitude", s.solar.longitudeDegrees}, {"year", s.solar.year}, {"month", s.solar.month},
          {"day", s.solar.day}, {"utcOffsetMinutes", s.solar.utcOffsetMinutes}, {"modelNorthDegrees", s.northDegrees},
          {"startMinute", s.startMinute}, {"endMinute", s.endMinute}, {"intervalMinutes", s.stepMinutes}};
}

} // namespace

bool RendererFrontend::bimSunHoursMatchesScene() const {
  const auto* model = subs_.bimManager.get();
  return model && model == bimSunHours_.model && model->hasScene() &&
      model->geometryRevision() == bimSunHours_.geometryRevision &&
      model->metadataRevision() == bimSunHours_.metadataRevision &&
      model->transformRevision() == bimSunHours_.transformRevision;
}

void RendererFrontend::startBimSunHours(const BimSunHoursSettings& settings) {
  pollBimSunHours();
  if (bimSunHours_.job.valid()) throw std::runtime_error("The previous sun study is still stopping");
  if (!subs_.bimManager || !subs_.bimManager->hasScene()) throw std::runtime_error("Open a BIM model before running a sun study");
  const auto& model = *subs_.bimManager;
  auto& study = bimSunHours_;
  study.model = &model;
  study.geometryRevision = model.geometryRevision();
  study.metadataRevision = model.metadataRevision();
  study.transformRevision = model.transformRevision();
  study.result.reset();
  study.captureSelection.reset();
  study.guiDirty = true;
  try {
    ValidateBimSunHoursSettings(settings);
    auto triangles = snapshotBimSunHours(model);
    study.stop = std::stop_source{};
    study.job = std::async(std::launch::async,
        [settings, triangles = std::move(triangles), stop = study.stop.get_token()] {
          return RunBimSunHours(settings, triangles, stop);
        });
    study.status = BimSunHoursState::Status::Running;
    study.message = "Calculating direct sun hours";
  } catch (const std::exception& error) {
    study.status = BimSunHoursState::Status::Failed;
    study.message = error.what();
  }
}

void RendererFrontend::cancelBimSunHours() {
  auto& study = bimSunHours_;
  study.stop.request_stop();
  study.result.reset();
  study.captureSelection.reset();
  study.status = BimSunHoursState::Status::Cancelled;
  study.message = "Sun study cancelled";
  study.guiDirty = true;
}

void RendererFrontend::pollBimSunHours(bool wait) {
  auto& study = bimSunHours_;
  if (study.model && !bimSunHoursMatchesScene()) {
    study.stop.request_stop();
    study.model = nullptr;
    study.result.reset();
    study.captureSelection.reset();
    study.status = BimSunHoursState::Status::Idle;
    study.message = "Model geometry or placement changed";
    study.guiDirty = true;
  }
  if (!study.job.valid() || (!wait && study.job.wait_for(std::chrono::seconds(0)) != std::future_status::ready)) return;
  const bool publish = study.status == BimSunHoursState::Status::Running && bimSunHoursMatchesScene();
  try {
    auto result = study.job.get();
    if (publish && !result.cancelled) {
      study.result = std::make_shared<const BimSunHoursResult>(std::move(result));
      study.status = BimSunHoursState::Status::Complete;
      study.message = "Sun study complete";
    } else if (publish) {
      cancelBimSunHours();
    }
  } catch (const std::exception& error) {
    if (publish) {
      study.status = BimSunHoursState::Status::Failed;
      study.message = error.what();
    }
  }
  study.guiDirty = true;
}

void RendererFrontend::publishBimSunHoursUi() {
  auto& study = bimSunHours_;
  if (!subs_.guiManager || !std::exchange(study.guiDirty, false)) return;
  const bool running = study.job.valid();
  subs_.guiManager->setBimSunHoursState({.running = running,
      .status = running && study.status != BimSunHoursState::Status::Running ? "Stopping sun study" : study.message,
      .result = study.result});
}

void RendererFrontend::processBimSunHoursUi() {
  if (!subs_.guiManager) return;
  auto& gui = *subs_.guiManager;
  try {
    if (gui.consumeBimSunHoursCancelRequest()) cancelBimSunHours();
    if (const auto request = gui.consumeBimSunHoursRunRequest()) startBimSunHours(*request);
    if (const auto path = gui.consumeBimSunHoursExportRequest()) {
      saveBimSunHours(container::util::pathFromUtf8(*path));
      gui.setStatusMessage("Saved sun study to " + *path);
    }
  } catch (const std::exception& error) {
    gui.setStatusMessage(error.what());
  }
}

bool RendererFrontend::bimSunHoursOverlayVisible() const {
  return bimSunHours_.result && bimSunHoursMatchesScene() &&
      bimSunHours_.captureVisibility.value_or(!subs_.guiManager || subs_.guiManager->bimSunHoursViewportVisible());
}

void RendererFrontend::prepareBimSunHoursOverlay(BimSunHoursOverlay& overlay,
                                                const std::function<void()>& retireReaders) {
  auto& study = bimSunHours_;
  const auto result = bimSunHoursMatchesScene() ? study.result : nullptr;
  const bool visible = bimSunHoursOverlayVisible();
  if (study.overlayResult != result) {
    if (study.overlayVertices.buffer != VK_NULL_HANDLE) {
      retireReaders();
      svc_.allocationManager.destroyBuffer(study.overlayVertices);
      study.overlayVertices = {};
    }
    study.overlayResult.reset();
    study.overlayVertexCount = 0;
    if (visible) {
      const auto vertices = sunHoursVertices(*result);
      study.overlayVertices = svc_.allocationManager.uploadBuffer(std::as_bytes(std::span(vertices)),
          VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
      study.overlayVertexCount = static_cast<uint32_t>(vertices.size());
      study.overlayResult = result;
    }
  }
  if (visible && study.overlayVertexCount) {
    overlay = {{study.overlayVertices.buffer, 0, VkDeviceSize(study.overlayVertexCount) * sizeof(BimSunHoursVertex)},
               study.overlayVertexCount, buffers_.cameraData.unjitteredViewProj};
  }
}

void recordBimSunHoursOverlay(VkCommandBuffer cmd, const FrameRecordParams& frame) {
  const auto& overlay = frame.bim.sunHours;
  if (frame.screenshot.sceneLinear || !overlay.vertexCount || overlay.vertices.buffer == VK_NULL_HANDLE) return;
  // The two raster paths and Path Tracing share the same display scope format.
  const auto pipeline = frame.pipelineHandle(RenderTechniqueId::DeferredRaster, kBimSunHoursPipeline);
  const auto layout = frame.pipelineLayout(RenderTechniqueId::DeferredRaster, kBimSunHoursPipeline);
  if (pipeline == VK_NULL_HANDLE || layout == VK_NULL_HANDLE) return;
  const BimSunHoursOverlayPushConstants constants{overlay.viewProjection, frame.postProcess.encodeSrgbOutput ? 1u : 0u};
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
  vkCmdBindVertexBuffers(cmd, 0, 1, &overlay.vertices.buffer, &overlay.vertices.offset);
  vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                     0, offsetof(BimSunHoursOverlayPushConstants, encodeSrgb) + sizeof(uint32_t), &constants);
  vkCmdDraw(cmd, overlay.vertexCount, 1, 0, 0);
}

void RendererFrontend::saveBimSunHours(const std::filesystem::path& path) const {
  const auto& result = bimSunHours_.result;
  if (!result || !bimSunHoursMatchesScene() || path.empty())
    throw std::runtime_error("Complete a sun study and provide a CSV path");
  const auto& s = result->settings;
  std::ofstream csv(path, std::ios::binary);
  csv.imbue(std::locale::classic());
  csv << std::setprecision(std::numeric_limits<double>::max_digits10)
      << "column,row,x,y,z,sun_hours,min_x,max_x,min_z,max_z,columns,rows,latitude,longitude,year,month,day,"
         "utc_offset_minutes,start_minute,end_minute,interval_minutes,model_north_degrees,sample_count,"
         "daylight_sample_count,daylight_hours,skipped_triangles\n";
  for (uint32_t row = 0; row < s.rows; ++row) for (uint32_t column = 0; column < s.columns; ++column) {
    const auto at = BimSunHoursCellCenter(s, column, row);
    csv << column << ',' << row << ',' << at.x << ',' << at.y << ',' << at.z << ','
        << result->hours[size_t(row) * s.columns + column] << ','
        << s.minX << ',' << s.maxX << ',' << s.minZ << ',' << s.maxZ << ',' << s.columns << ',' << s.rows << ','
        << s.solar.latitudeDegrees << ',' << s.solar.longitudeDegrees << ',' << s.solar.year << ',' << s.solar.month << ','
        << s.solar.day << ',' << s.solar.utcOffsetMinutes << ',' << s.startMinute << ',' << s.endMinute << ',' << s.stepMinutes << ','
        << s.northDegrees << ',' << result->sampleCount << ',' << result->daylightSampleCount << ',' << result->daylightHours << ','
        << result->skippedTriangles << '\n';
  }
  csv.close();
  if (!csv) throw std::runtime_error("Could not write the sun study CSV");
}

void RendererFrontend::applyCaptureBimSunHours(const container::temporal::CaptureBimSunHours& request) {
  using Action = container::temporal::CaptureBimSunHours::Action;
  switch (request.action) {
  case Action::Start:
    startBimSunHours(request.settings);
    if (request.wait) pollBimSunHours(true);
    break;
  case Action::Cancel: cancelBimSunHours(); break;
  case Action::Wait: pollBimSunHours(true); break;
  case Action::Overlay: bimSunHours_.captureVisibility = request.visible; break;
  case Action::Select: {
    const auto& result = bimSunHours_.result;
    if (!result || !bimSunHoursMatchesScene() || request.column >= result->settings.columns ||
        request.row >= result->settings.rows) throw std::runtime_error("The requested sun study cell is unavailable");
    bimSunHours_.captureSelection = size_t(request.row) * result->settings.columns + request.column;
    break;
  }
  case Action::Export: saveBimSunHours(container::util::pathFromUtf8(request.path)); break;
  }
}

void RendererFrontend::appendBimSunHoursTelemetry(nlohmann::json& json) const {
  const auto& study = bimSunHours_;
  if (!study.model && study.message.empty() && (!subs_.bimManager || !subs_.bimManager->hasScene())) return;
  const auto status = [&] {
    switch (study.status) {
    case BimSunHoursState::Status::Running: return "running";
    case BimSunHoursState::Status::Complete: return "complete";
    case BimSunHoursState::Status::Cancelled: return "cancelled";
    case BimSunHoursState::Status::Failed: return "failed";
    default: return "idle";
    }
  };
  auto& data = json["bimSunHours"];
  data = {{"status", status()}, {"message", study.message}, {"settings", nullptr},
          {"hours", nlohmann::json::array()}, {"selected", nullptr}, {"sampleCount", 0},
          {"daylightSampleCount", 0}, {"daylightHours", 0}, {"skippedTriangles", 0},
          {"overlayVisible", bimSunHoursOverlayVisible()}};
  if (!study.result || !bimSunHoursMatchesScene()) return;
  const auto& result = *study.result;
  data["settings"] = sunStudySettings(result.settings);
  data["hours"] = result.hours;
  data["sampleCount"] = result.sampleCount;
  data["daylightSampleCount"] = result.daylightSampleCount;
  data["daylightHours"] = result.daylightHours;
  data["skippedTriangles"] = result.skippedTriangles;
  auto selection = study.captureSelection;
  if (!selection && subs_.guiManager) selection = subs_.guiManager->bimSunHoursSelectedCell();
  if (selection && *selection < result.hours.size()) {
    const auto at = BimSunHoursCellCenter(result.settings, *selection);
    data["selected"] = {{"column", *selection % result.settings.columns}, {"row", *selection / result.settings.columns},
                       {"x", at.x}, {"y", at.y}, {"z", at.z}, {"hours", result.hours[*selection]}};
  }
}

} // namespace container::renderer
