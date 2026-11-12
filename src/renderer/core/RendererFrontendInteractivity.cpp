#include <Container/renderer/core/RendererFrontend.h>

#include <Container/renderer/lighting/DdgiManager.h>
#include <Container/renderer/temporal/TemporalCapture.h>
#include <Container/utility/GuiManager.h>
#include <Container/utility/SceneManager.h>
#include <Container/utility/SwapChainManager.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace container::renderer {
using namespace container::geometry::gltf;

void RendererFrontend::applyCaptureViewportPointer(const container::temporal::CaptureViewportPointer& pointer) {
  const auto extent = svc_.swapChainManager.extent();
  if (pointer.pixel.x >= extent.width || pointer.pixel.y >= extent.height)
    throw std::invalid_argument("Capture viewportPointer pixel is outside the completed viewport");
  using Action = container::temporal::CaptureViewportPointer::Action;
  if (pointer.action == Action::Select)
    selectMeshNodeAtCursor(pointer.pixel.x, pointer.pixel.y);
  else
    hoverMeshNodeAtCursor(pointer.pixel.x, pointer.pixel.y);
}

void RendererFrontend::resetGltfInteractivity() {
  gltfInteractivity_.reset();
  gltfInteractivityError_.clear();
  gltfInteractivityHoverChain_.clear();
  hoverPickCache_.valid = false;
  if (!subs_.sceneManager) return;
  try {
    auto player = std::make_unique<GltfInteractivity>(subs_.sceneManager->animationSource());
    if (player->available()) gltfInteractivity_ = std::move(player);
  } catch (const std::invalid_argument& error) {
    gltfInteractivityError_ = error.what();
  }
}

void RendererFrontend::pauseGltfInteractivity() {
  if (gltfInteractivity_) gltfInteractivity_->setPlaying(false);
  gltfInteractivityHoverChain_.clear();
  hoverPickCache_.valid = false;
  gltfInteractivityRequest_ = {};
  if (subs_.guiManager) {
    auto& controls = subs_.guiManager->gltfAnimationControls();
    controls.requestedInteractivityPlaying.reset();
    controls.requestedInteractivityEvent.reset();
    controls.requestedInteractivityReset = false;
  }
}

AnimationDirtySet RendererFrontend::updateGltfInteractivity(double delta, bool& discontinuity) {
  auto request = std::exchange(gltfInteractivityRequest_, {});
  auto* controls = subs_.guiManager ? &subs_.guiManager->gltfAnimationControls() : nullptr;
  if (controls) {
    if (auto playing = std::exchange(controls->requestedInteractivityPlaying, {})) request.playing = playing;
    if (auto event = std::exchange(controls->requestedInteractivityEvent, {})) request.event = std::move(event);
    request.reset |= std::exchange(controls->requestedInteractivityReset, false);
  }
  AnimationDirtySet dirty;
  if (gltfInteractivity_) {
    auto& player = *gltfInteractivity_;
    auto& source = subs_.sceneManager->animationSource();
    if (request.reset) {
      dirty = player.reset(source);
      player.setPlaying(false);
      gltfInteractivityHoverChain_.clear();
      hoverPickCache_.valid = false;
      discontinuity = true;
    }
    if (request.playing) {
      if (player.playing() != *request.playing) {
        gltfInteractivityHoverChain_.clear();
        hoverPickCache_.valid = false;
      }
      player.setPlaying(*request.playing);
    }
    gltfInteractivityError_.clear();
    if (request.event && !player.queueCustomEvent(*request.event))
      gltfInteractivityError_ = "Unknown custom event or paused behavior graph";
    dirty.merge(player.advance(source, gltfAnimation_->library(), delta));
    if (!player.diagnostic().empty()) gltfInteractivityError_ = player.diagnostic();
    if (!dirty.empty() && subs_.ddgiManager) subs_.ddgiManager->synchronizeAsync();
  } else if (request.playing || request.reset || request.event) {
    if (gltfInteractivityError_.empty()) gltfInteractivityError_ = "No supported behavior graph in the active asset";
  }
  if (controls) {
    controls->interactivityAvailable = bool(gltfInteractivity_);
    controls->interactivityPlaying = gltfInteractivity_ && gltfInteractivity_->playing();
    controls->interactivityTime = gltfInteractivity_ ? gltfInteractivity_->time() : 0;
    controls->interactivityDiagnostic = gltfInteractivityError_;
    if (gltfInteractivity_ && controls->interactivityEvents.empty())
      controls->interactivityEvents = gltfInteractivity_->customEvents();
  }
  return dirty;
}

std::vector<size_t> RendererFrontend::gltfInteractivityAncestors(uint32_t index) const {
  std::vector<size_t> chain;
  for (size_t depth = 0; depth < 96 && index != container::scene::SceneGraph::kInvalidNode; ++depth) {
    const auto* node = sceneGraph_.getNode(index);
    if (!node || node->interactionRootBoundary || !sceneGraph_.isNodeActive(index)) break;
    if (node->sourceNodeIndex >= 0) chain.push_back(size_t(node->sourceNodeIndex));
    index = node->parent;
  }
  return chain;
}

void RendererFrontend::queueGltfInteractivitySelection(uint32_t node, std::optional<glm::vec3> point) {
  if (!gltfInteractivity_ || !gltfInteractivity_->playing()) return;
  const auto origin = buffers_.cameraData.cameraWorldPosition;
  std::optional<GltfInteractivity::Point> hit, rayOrigin;
  if (point) {
    hit = GltfInteractivity::Point{point->x, point->y, point->z};
    rayOrigin = GltfInteractivity::Point{origin.x, origin.y, origin.z};
  }
  gltfInteractivity_->queueNodeEvent(GltfInteractivity::NodeEvent::Select,
      gltfInteractivityAncestors(node), hit, rayOrigin);
}

void RendererFrontend::updateGltfInteractivityHover(uint32_t node) {
  if (!gltfInteractivity_ || !gltfInteractivity_->playing()) return;
  auto next = gltfInteractivityAncestors(node);
  const auto& previous = gltfInteractivityHoverChain_;
  if (next == previous) return;
  // Only ancestors the pointer actually entered or left receive hover events.
  const auto [oldTail, newTail] = std::mismatch(previous.rbegin(), previous.rend(), next.rbegin(), next.rend());
  gltfInteractivity_->queueNodeEvent(GltfInteractivity::NodeEvent::HoverOut,
      {previous.begin(), oldTail.base()});
  gltfInteractivity_->queueNodeEvent(GltfInteractivity::NodeEvent::HoverIn,
      {next.begin(), newTail.base()});
  gltfInteractivityHoverChain_ = std::move(next);
}

void RendererFrontend::appendGltfInteractivityTelemetry(nlohmann::json& json) const {
  json["gltfInteractivity"] = {{"available", bool(gltfInteractivity_)}, {"diagnostic", gltfInteractivityError_}};
  if (!gltfInteractivity_) return;
  auto& state = json["gltfInteractivity"];
  state["playing"] = gltfInteractivity_->playing();
  state["started"] = gltfInteractivity_->started();
  state["time"] = gltfInteractivity_->time();
  state["activeClips"] = gltfInteractivity_->activeClips();
  state["customEvents"] = gltfInteractivity_->customEvents();
}

} // namespace container::renderer
