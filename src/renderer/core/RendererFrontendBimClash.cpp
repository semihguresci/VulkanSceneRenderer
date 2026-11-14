#include "Container/renderer/core/RendererFrontend.h"

#include "Container/renderer/bim/BimManager.h"
#include "Container/renderer/scene/CameraController.h"
#include "Container/renderer/temporal/TemporalCapture.h"
#include "Container/utility/BcfViewpoint.h"
#include "Container/utility/Platform.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <fstream>
#include <stdexcept>
#include <unordered_map>

namespace container::renderer {
namespace {

std::string bimElementId(const BimElementMetadataView& element) {
  if (!element.guid.empty()) return std::string(element.guid);
  if (!element.sourceId.empty()) return std::string(element.sourceId);
  return "Object " + std::to_string(element.objectIndex);
}

double cubicMetres(const BimManager& model, double volume) {
  // The loader already converted the source length units. Undo the optional
  // display scale, keeping native element transforms in the measured volume.
  const auto& units = model.modelUnitMetadata();
  const double scale = units.hasImportScale ? units.importScale : 1.0;
  return volume / (scale * scale * scale);
}

using ClashGroupMembers = std::vector<container::ui::BimHardClashElementRef>;

std::vector<uint8_t> clashMembership(const BimManager& model,
                                    const container::ui::BimHardClashRunRequest& request) {
  std::vector<uint8_t> membership(model.objectData().size());
  const auto addGroup = [&](const container::ui::BimHardClashGroup& group, uint8_t bit) {
    if (const auto* category = std::get_if<std::string>(&group.members)) {
      for (const auto& element : model.elementMetadata()) {
        if (element.type == *category) membership.at(element.objectIndex) |= bit;
      }
      return;
    }
    for (const auto& ref : std::get<ClashGroupMembers>(group.members)) {
      if (!ref.guid.empty() || !ref.sourceId.empty()) {
        const auto objects = !ref.guid.empty() ? model.objectIndicesForGuid(ref.guid)
                                               : model.objectIndicesForSourceId(ref.sourceId);
        for (const auto object : objects) membership.at(object) |= bit;
      } else if (model.metadataForObject(ref.objectIndex)) {
        membership.at(ref.objectIndex) |= bit;
      }
    }
  };
  addGroup(request.first, kBimHardClashGroupA);
  addGroup(request.second, kBimHardClashGroupB);
  return membership;
}

std::vector<BimHardClashElement> snapshotClashElements(
    const BimManager& model, const container::ui::BimHardClashRunRequest& request) {
  const BimHardClashLimits limits;
  const auto membership = clashMembership(model, request);
  constexpr size_t unused = std::numeric_limits<size_t>::max();
  std::vector<size_t> objectToElement(membership.size(), unused);
  std::vector<BimHardClashElement> elements;
  std::unordered_map<uint64_t, size_t> byProduct;
  const auto productKey = [](const BimElementMetadataView& element) -> uint64_t {
    return element.productIdentityId ? element.productIdentityId : (uint64_t{1} << 32) + element.objectIndex;
  };
  for (const auto& element : model.elementMetadata()) {
    const auto index = element.objectIndex;
    const auto groups = membership[index];
    if (!groups) continue;
    const auto entry = byProduct.try_emplace(productKey(element), elements.size());
    if (entry.second) {
      if (elements.size() == limits.maxElements)
        throw std::runtime_error("Select fewer elements to fit the clash check memory limit");
      elements.push_back({.objectIndex = index});
    }
    elements[entry.first->second].groups |= groups;
  }
  for (const auto& element : model.elementMetadata()) {
    if (element.geometryKind != BimGeometryKind::Mesh) continue;
    const auto product = byProduct.find(productKey(element));
    if (product != byProduct.end()) objectToElement[element.objectIndex] = product->second;
  }

  const auto vertices = model.vertices();
  const auto indices = model.indices();
  size_t totalTriangles = 0;
  const auto append = [&](const DrawCommand& draw, uint32_t objectIndex) {
    const auto destination = objectToElement.at(objectIndex);
    if (destination == unused) return;
    auto& element = elements[destination];
    if (!element.unavailableReason.empty()) return;
    const auto first = size_t(draw.firstIndex);
    const auto count = size_t(draw.indexCount);
    if (first > indices.size() || count > indices.size() - first || count % 3) {
      element.unavailableReason = "Invalid triangle index range";
      return;
    }
    if (count / 3 > limits.maxTrianglesPerElement - element.triangles.size()) {
      element.unavailableReason = "Element exceeds the clash mesh size limit";
      return;
    }
    if (count / 3 > limits.maxTriangles - totalTriangles)
      throw std::runtime_error("Select fewer elements to fit the clash geometry memory limit");
    const glm::dmat4 world(model.objectData()[objectIndex].model);
    const bool reflected = glm::determinant(glm::dmat3(world)) < 0.0;
    std::unordered_map<uint32_t, uint32_t> localIndices;
    for (size_t offset = first; offset < first + count; offset += 3) {
      std::array<uint32_t, 3> triangle{};
      for (size_t corner = 0; corner < triangle.size(); ++corner) {
        const auto vertex = indices[offset + corner];
        if (vertex >= vertices.size()) {
          element.unavailableReason = "Triangle references a missing vertex";
          return;
        }
        const auto mapped = localIndices.try_emplace(vertex, static_cast<uint32_t>(element.vertices.size()));
        if (mapped.second) {
          if (element.vertices.size() == limits.maxVerticesPerElement) {
            element.unavailableReason = "Element exceeds the clash mesh size limit";
            return;
          }
          element.vertices.emplace_back(world * glm::dvec4(vertices[vertex].position, 1.0));
        }
        triangle[corner] = mapped.first->second;
      }
      if (reflected) std::swap(triangle[1], triangle[2]);
      element.triangles.push_back(triangle);
      ++totalTriangles;
    }
  };
  // The unfiltered mesh draws retain one geometry range per instance. A BIM
  // product can have several draw rows with different materials or matrices.
  for (const auto* draws : {&model.opaqueDrawCommands(), &model.transparentDrawCommands()}) {
    for (const auto& draw : *draws) {
      for (uint32_t instance = 0; instance < draw.instanceCount; ++instance)
        append(draw, draw.objectIndex + instance);
    }
  }
  for (auto& element : elements) {
    if (element.triangles.empty() && element.unavailableReason.empty())
      element.unavailableReason = "Element has no solid triangle geometry";
  }
  return elements;
}

}  // namespace

bool RendererFrontend::bimClashMatchesScene() const {
  const auto* model = subs_.bimManager.get();
  return model && model == bimClash_.model && model->hasScene() &&
      model->geometryRevision() == bimClash_.geometryRevision &&
      model->metadataRevision() == bimClash_.metadataRevision &&
      model->transformRevision() == bimClash_.transformRevision;
}

void RendererFrontend::startBimClash(const container::ui::BimHardClashRunRequest& request) {
  pollBimClash();
  if (bimClash_.job.valid()) throw std::runtime_error("The previous clash check is still stopping");
  if (!subs_.bimManager || !subs_.bimManager->hasScene())
    throw std::runtime_error("Open a BIM model before checking for clashes");
  const auto& model = *subs_.bimManager;
  bimClash_.result = {};
  bimClash_.selected.reset();
  bimClash_.guiDirty = true;
  bimClash_.model = &model;
  bimClash_.geometryRevision = model.geometryRevision();
  bimClash_.metadataRevision = model.metadataRevision();
  bimClash_.transformRevision = model.transformRevision();
  try {
    auto elements = snapshotClashElements(model, request);
    bimClash_.stop = std::stop_source{};
    bimClash_.job = std::async(std::launch::async,
        [elements = std::move(elements), stop = bimClash_.stop.get_token()] {
          return FindBimHardClashes(elements, stop);
        });
    bimClash_.status = BimClashState::Status::Running;
    bimClash_.message = "Checking for hard clashes";
  } catch (const std::exception& error) {
    bimClash_.status = BimClashState::Status::Failed;
    bimClash_.message = error.what();
  }
}

void RendererFrontend::cancelBimClash() {
  bimClash_.stop.request_stop();
  bimClash_.result = {};
  bimClash_.selected.reset();
  bimClash_.status = BimClashState::Status::Cancelled;
  bimClash_.message = "Clash check cancelled";
  bimClash_.guiDirty = true;
}

void RendererFrontend::pollBimClash(bool wait) {
  if (bimClash_.model && !bimClashMatchesScene()) {
    bimClash_.stop.request_stop();
    bimClash_.model = nullptr;
    bimClash_.result = {};
    bimClash_.selected.reset();
    bimClash_.status = BimClashState::Status::Idle;
    bimClash_.message = "Model geometry or placement changed";
    bimClash_.guiDirty = true;
  }
  if (!bimClash_.job.valid() || (!wait &&
      bimClash_.job.wait_for(std::chrono::seconds(0)) != std::future_status::ready)) return;
  const bool publish = bimClash_.status == BimClashState::Status::Running && bimClashMatchesScene();
  try {
    auto result = bimClash_.job.get();
    if (publish && !result.cancelled) {
      bimClash_.result = std::move(result);
      bimClash_.status = BimClashState::Status::Complete;
      bimClash_.message = "Clash check complete";
    } else if (publish) {
      cancelBimClash();
    }
  } catch (const std::exception& error) {
    if (publish) {
      bimClash_.status = BimClashState::Status::Failed;
      bimClash_.message = error.what();
    }
  }
  bimClash_.guiDirty = true;
}

void RendererFrontend::publishBimClashUi() {
  if (!subs_.guiManager || !std::exchange(bimClash_.guiDirty, false)) return;
  container::ui::BimHardClashUiState state;
  state.running = bimClash_.job.valid();
  state.status = state.running && bimClash_.status != BimClashState::Status::Running
      ? "Stopping clash check" : bimClash_.message;
  state.scannedPairs = bimClash_.result.checkedPairs;
  state.invalidElements = bimClash_.result.skipped.size();
  state.failedPairs = bimClash_.result.failedPairs.size();
  state.pairLimitReached = bimClash_.result.pairLimitReached;
  if (bimClashMatchesScene()) {
    const auto& model = *bimClash_.model;
    const auto metadata = model.elementMetadata();
    state.rows.reserve(bimClash_.result.hits.size());
    for (const auto& hit : bimClash_.result.hits) {
      state.rows.push_back({hit.firstObjectIndex, hit.secondObjectIndex,
          bimElementId(metadata[hit.firstObjectIndex]),
          bimElementId(metadata[hit.secondObjectIndex]), cubicMetres(model, hit.volume)});
    }
    state.skipped.reserve(bimClash_.result.skipped.size());
    for (const auto& element : bimClash_.result.skipped) {
      state.skipped.push_back({bimElementId(metadata[element.objectIndex]), element.reason});
    }
  }
  subs_.guiManager->setBimHardClashState(std::move(state));
}

void RendererFrontend::processBimClashUi() {
  if (!subs_.guiManager) return;
  auto& gui = *subs_.guiManager;
  try {
    if (gui.consumeBimHardClashCancelRequest()) cancelBimClash();
    if (const auto request = gui.consumeBimHardClashRunRequest()) startBimClash(*request);
    if (const auto row = gui.consumeBimHardClashSelectRequest()) selectBimClash(*row);
    if (const auto request = gui.consumeBimHardClashSaveViewpointRequest()) {
      const auto path = container::util::pathFromUtf8(request->path);
      saveBimClashViewpoint(request->rowIndex, path);
      gui.setStatusMessage("Saved clash viewpoint to " + request->path);
    }
  } catch (const std::exception& error) {
    gui.setStatusMessage(error.what());
  }
}

void RendererFrontend::selectBimClash(size_t row) {
  if (!bimClashMatchesScene() || row >= bimClash_.result.hits.size() || !subs_.cameraController)
    throw std::runtime_error("The selected clash is no longer available");
  const auto& hit = bimClash_.result.hits[row];
  const auto center = glm::vec3((hit.boundsMin + hit.boundsMax) * 0.5);
  const auto radius = static_cast<float>(glm::length(hit.boundsMax - hit.boundsMin) * 0.5);
  selectBimObject(hit.firstObjectIndex);
  subs_.cameraController->setSelectionPivotOverride({.center = center, .radius = radius, .valid = true});
  subs_.cameraController->frameNodeOrScene(container::scene::SceneGraph::kInvalidNode);
  refreshCameraData();
  bimClash_.selected = row;
}

bool RendererFrontend::selectedBimClashMarker(BimCoordinationOverlayClashPair& marker) const {
  if (!bimClash_.selected || !bimClashMatchesScene()) return false;
  const auto& hit = bimClash_.result.hits[*bimClash_.selected];
  marker = {.primaryObjectIndex = hit.firstObjectIndex, .secondaryObjectIndex = hit.secondObjectIndex,
            .hasPosition = true, .position = glm::vec3(hit.position), .label = "Hard clash"};
  return true;
}

void RendererFrontend::saveBimClashViewpoint(size_t row, const std::filesystem::path& path) {
  if (!bimClashMatchesScene() || bimClash_.selected != row || path.empty())
    throw std::runtime_error("Select a clash and provide a BCF viewpoint path");
  const auto& hit = bimClash_.result.hits[row];
  const auto first = bimClash_.model->elementMetadata()[hit.firstObjectIndex];
  const auto second = bimClash_.model->elementMetadata()[hit.secondObjectIndex];
  auto snapshot = currentViewpointSnapshot();
  snapshot.label = "Hard clash " + bimElementId(first) + " / " + bimElementId(second);
  snapshot.selectedBimObjectIndex = first.objectIndex;
  snapshot.selectedBimGuid = first.guid;
  snapshot.selectedBimSourceId = first.sourceId;
  snapshot.selectedBimType = first.type;
  snapshot.additionalBimSelections.push_back({std::string(second.guid), std::string(second.sourceId)});
  container::ui::bcf::BcfViewpointMarkup markup;
  markup.pins.push_back({.guid = container::ui::bcf::StablePinGuid(first.guid, first.sourceId, snapshot.label),
                         .label = snapshot.label, .hasLocation = true, .location = glm::vec3(hit.position),
                         .ifcGuid = std::string(first.guid), .sourceId = std::string(first.sourceId)});
  std::ofstream output(path, std::ios::binary);
  output << container::ui::bcf::ExportVisualizationInfo(snapshot, markup);
  output.close();
  if (!output) throw std::runtime_error("Could not write the BCF clash viewpoint");
}

void RendererFrontend::applyCaptureBimClash(const container::temporal::CaptureBimClash& request) {
  using Action = container::temporal::CaptureBimClash::Action;
  switch (request.action) {
  case Action::Start: {
    const auto group = [](const std::string& category, const std::vector<std::string>& guids)
        -> container::ui::BimHardClashGroup {
      if (!category.empty()) return {category};
      ClashGroupMembers members;
      members.reserve(guids.size());
      for (const auto& guid : guids) members.push_back({.guid = guid});
      return {std::move(members)};
    };
    startBimClash({group(request.categoryA, request.guidsA), group(request.categoryB, request.guidsB)});
    if (request.wait) pollBimClash(true);
    break;
  }
  case Action::Cancel: cancelBimClash(); break;
  case Action::Wait: pollBimClash(true); break;
  case Action::Select:
    selectBimClash(request.result);
    captureBimAnnotations_.reset();
    break;
  case Action::Export:
    if (!bimClash_.selected) throw std::runtime_error("Capture BIM clash export requires a selected result");
    saveBimClashViewpoint(*bimClash_.selected, container::util::pathFromUtf8(request.path));
    break;
  }
}

void RendererFrontend::appendBimClashTelemetry(nlohmann::json& json) const {
  if (!bimClash_.model && bimClash_.message.empty() && (!subs_.bimManager || !subs_.bimManager->hasScene())) return;
  const auto status = [&] {
    switch (bimClash_.status) {
    case BimClashState::Status::Running: return "running";
    case BimClashState::Status::Complete: return "complete";
    case BimClashState::Status::Cancelled: return "cancelled";
    case BimClashState::Status::Failed: return "failed";
    default: return "idle";
    }
  };
  auto& state = json["bimClash"];
  state = {{"status", status()}, {"message", bimClash_.message},
           {"checkedPairs", 0}, {"failedPairs", 0}, {"skippedElements", 0},
           {"pairLimitReached", false}, {"selected", nullptr}, {"markerDrawCount", 0},
           {"pairs", nlohmann::json::array()}};
  if (!bimClashMatchesScene()) return;
  const auto& model = *bimClash_.model;
  state["checkedPairs"] = bimClash_.result.checkedPairs;
  state["failedPairs"] = bimClash_.result.failedPairs.size();
  state["skippedElements"] = bimClash_.result.skipped.size();
  state["pairLimitReached"] = bimClash_.result.pairLimitReached;
  if (bimClash_.selected) {
    state["selected"] = *bimClash_.selected;
    if (!captureBimAnnotations_)
      state["markerDrawCount"] = model.coordinationMarkerDrawData().clashDrawCommands.size();
  }
  for (const auto& hit : bimClash_.result.hits) {
    const auto first = model.elementMetadata()[hit.firstObjectIndex];
    const auto second = model.elementMetadata()[hit.secondObjectIndex];
    state["pairs"].push_back({{"objects", {bimElementId(first), bimElementId(second)}},
        {"objectIndices", {hit.firstObjectIndex, hit.secondObjectIndex}}, {"volume", cubicMetres(model, hit.volume)},
        {"position", {hit.position.x, hit.position.y, hit.position.z}}});
  }
}

}  // namespace container::renderer
