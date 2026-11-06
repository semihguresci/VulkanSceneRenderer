#include <Container/renderer/core/RendererFrontend.h>

#include <Container/ecs/World.h>
#include <Container/geometry/GltfAnimationRuntime.h>
#include <Container/renderer/lighting/DdgiManager.h>
#include <Container/renderer/lighting/LightingManager.h>
#include <Container/renderer/scene/SceneController.h>
#include <Container/renderer/temporal/TemporalManager.h>
#include <Container/renderer/temporal/TemporalCapture.h>
#include <Container/utility/GuiManager.h>
#include <Container/utility/SceneManager.h>
#include <Container/utility/SwapChainManager.h>

#include <nlohmann/json.hpp>

#include <cmath>
#include <exception>
#include <set>
#include <stdexcept>
#include <utility>

namespace container::renderer {
void RendererFrontend::selectGltfCamera(std::optional<size_t> node) {
  if (node == gltfAnimationCameraNode_) return;
  if (node) {
    const auto extent = svc_.swapChainManager.extent();
    if (!container::geometry::gltf::EvaluateAnimatedCamera(
        subs_.sceneManager->animationSource(), sceneGraph_, *node,
        extent.height ? float(extent.width) / float(extent.height) : 1.0f))
      throw std::invalid_argument("The selected glTF node has no camera in this scene");
  }
  gltfAnimationCameraNode_ = node;
  if (subs_.temporalManager) subs_.temporalManager->reset("authored glTF camera selection");
}

void RendererFrontend::resetGltfAnimation(bool applyStartupOptions) {
  gltfAnimation_ = subs_.sceneManager->takePreparedAnimation();
  gltfAnimationCameraNode_.reset();
  gltfAnimationCamera_.reset();
  gltfAnimationSourceRevision_ = subs_.sceneManager->animationSourceRevision();
  pendingGltfAnimationDeltaSeconds_ = 0;
  container::ui::GltfAnimationControls controls;
  if (applyStartupOptions && svc_.config.gltfCameraNode >= 0)
    selectGltfCamera(static_cast<size_t>(svc_.config.gltfCameraNode));
  if (applyStartupOptions && svc_.config.gltfAnimationClip >= 0) {
    if (!gltfAnimation_) throw std::invalid_argument("The model has no glTF animation clips");
    gltfAnimation_->setLoop(svc_.config.gltfAnimationLoop);
    gltfAnimation_->setSpeed(svc_.config.gltfAnimationSpeed);
    if (subs_.ddgiManager) subs_.ddgiManager->synchronizeAsync();
    auto& source = subs_.sceneManager->animationSource();
    const auto dirty = gltfAnimation_->selectClip(source, static_cast<size_t>(svc_.config.gltfAnimationClip),
                                                 svc_.config.gltfAnimationTime);
    if (svc_.config.gltfAnimationPlaying) gltfAnimation_->play();
    publishGltfAnimation(dirty, true);
  }
  if (gltfAnimation_) {
    for (size_t i = 0; i < gltfAnimation_->library().clips.size(); ++i) {
      const auto& clip = gltfAnimation_->library().clips[i];
      controls.clips.push_back({clip.name.empty() ? "Clip " + std::to_string(i + 1) : clip.name, clip.duration});
    }
    for (const auto& diagnostic : gltfAnimation_->library().diagnostics)
      controls.diagnostics.push_back("Clip " + std::to_string(diagnostic.clip + 1) + ", channel " +
          std::to_string(diagnostic.channel + 1) + ": " + diagnostic.message);
  }
  if (subs_.guiManager) subs_.guiManager->gltfAnimationControls() = std::move(controls);
  refreshGltfCameraChoices();
}

void RendererFrontend::refreshGltfCameraChoices() {
  if(!subs_.sceneManager || !subs_.guiManager) return;
  auto& choices=subs_.guiManager->gltfAnimationControls().cameras; choices.clear();
  const auto& source=subs_.sceneManager->animationSource();
  for(uint32_t graphIndex=0;graphIndex<sceneGraph_.nodeCount();++graphIndex) {
    const auto& runtime=*sceneGraph_.getNode(graphIndex);
    if (!sceneGraph_.isNodeActive(graphIndex) || runtime.sourceNodeIndex < 0) continue;
    const auto index=size_t(runtime.sourceNodeIndex);
    if(index>=source.nodes.size()) continue;
    const auto& node=source.nodes[index];
    if(node.camera<0 || size_t(node.camera)>=source.cameras.size()) continue;
    const auto& camera=source.cameras[size_t(node.camera)];
    std::string name=camera.name.empty()?"Camera "+std::to_string(node.camera+1):camera.name;
    if(!node.name.empty()) name+=" ("+node.name+")";
    choices.push_back({std::move(name),index});
  }
}
void RendererFrontend::updateGltfAnimation() {
  using namespace container::geometry::gltf;
  if (!subs_.sceneManager) return;
  if (gltfAnimationSourceRevision_ != subs_.sceneManager->animationSourceRevision())
    resetGltfAnimation(!gltfAnimationSourceRevision_);
  const double delta = std::exchange(pendingGltfAnimationDeltaSeconds_, 0.0);
  auto& source = subs_.sceneManager->animationSource();
  auto* controls = subs_.guiManager ? &subs_.guiManager->gltfAnimationControls() : nullptr;
  if (controls) {
    if (auto camera = std::exchange(controls->requestedCameraNode, {})) {
      try {
        if (*camera < -1) throw std::invalid_argument("Invalid authored camera selector");
        selectGltfCamera(*camera < 0 ? std::nullopt : std::optional<size_t>(size_t(*camera)));
        controls->error.clear();
      } catch (const std::exception& error) { controls->error = error.what(); }
    }
    controls->cameraNode = gltfAnimationCameraNode_ ? static_cast<int>(*gltfAnimationCameraNode_) : -1;
  }
  if (!gltfAnimation_) return;
  AnimationDirtySet dirty;
  bool discontinuity = false;
  try {
    if (controls) {
      if (auto clip = std::exchange(controls->requestedClip,{})) {
        if (subs_.ddgiManager) subs_.ddgiManager->synchronizeAsync();
        dirty.merge(*clip < 0 ? gltfAnimation_->clearClip(source) : gltfAnimation_->selectClip(source,static_cast<size_t>(*clip)));
        discontinuity = true;
      }
      if (auto speed = std::exchange(controls->requestedSpeed,{})) gltfAnimation_->setSpeed(*speed);
      if (auto loop = std::exchange(controls->requestedLoop,{})) gltfAnimation_->setLoop(*loop);
      if (auto playing = std::exchange(controls->requestedPlaying,{})) { if (*playing) gltfAnimation_->play(); else gltfAnimation_->pause(); }
      if (auto time = std::exchange(controls->requestedTime,{})) {
        if (subs_.ddgiManager) subs_.ddgiManager->synchronizeAsync();
        dirty.merge(gltfAnimation_->seek(source,*time)); discontinuity = true;
      }
    }
    const double previous = gltfAnimation_->time();
    if (gltfAnimation_->playing() && delta > 0 && !discontinuity) {
      if (subs_.ddgiManager) subs_.ddgiManager->synchronizeAsync();
      dirty.merge(gltfAnimation_->advance(source,delta));
      if (gltfAnimation_->loop() &&
          ((gltfAnimation_->selectedClip() && gltfAnimation_->speed() != 0 &&
            delta >= gltfAnimation_->library().clips[*gltfAnimation_->selectedClip()].duration/std::abs(gltfAnimation_->speed())) ||
           (gltfAnimation_->speed() > 0 && gltfAnimation_->time() < previous) ||
           (gltfAnimation_->speed() < 0 && gltfAnimation_->time() > previous))) discontinuity = true;
    }
    publishGltfAnimation(dirty, discontinuity);
    if (controls) controls->error.clear();
  } catch (const std::exception& error) {
    gltfAnimation_->pause();
    if (controls) controls->error = error.what();
    if (subs_.temporalManager) subs_.temporalManager->reset("glTF animation evaluation failed");
  }
  if (controls) {
    controls->selectedClip = gltfAnimation_->selectedClip() ? static_cast<int>(*gltfAnimation_->selectedClip()) : -1;
    controls->time = gltfAnimation_->time(); controls->speed = gltfAnimation_->speed();
    controls->playing = gltfAnimation_->playing(); controls->loop = gltfAnimation_->loop();
    controls->cameraNode = gltfAnimationCameraNode_ ? static_cast<int>(*gltfAnimationCameraNode_) : -1;
  }
}
void RendererFrontend::publishGltfAnimation(
    const container::geometry::gltf::AnimationDirtySet& dirty, bool discontinuity) {
  if (!dirty.empty()) {
    container::geometry::gltf::ApplyAnimatedNodeTransforms(
        subs_.sceneManager->animationSource(), dirty, sceneGraph_);
    subs_.sceneManager->publishAnimationChanges(dirty, sceneGraph_, sceneState_.rootNode);
    if (subs_.lightingManager && (!dirty.nodes.empty() || !dirty.lights.empty()))
      subs_.lightingManager->updateLightingData();
    refreshSceneObjectData();
    syncSceneProviders();
  }
  // Ordinary rigid motion retains the last submitted transform history.
  if (subs_.temporalManager && (discontinuity ||
      !dirty.materials.empty() || !dirty.lights.empty() || !dirty.cameras.empty()))
    subs_.temporalManager->reset("glTF animation timeline or shading changed");
}

void RendererFrontend::applyGltfAnimationCapture(const container::temporal::CaptureEvent& event) {
  if (!event.animationClip && !event.animationTime && !event.animationPlaying) return;
  if (!subs_.sceneManager) throw std::runtime_error("Capture has no glTF scene");
  if (gltfAnimationSourceRevision_ != subs_.sceneManager->animationSourceRevision())
    resetGltfAnimation(!gltfAnimationSourceRevision_);
  if (!gltfAnimation_) throw std::runtime_error("Capture scene has no animation clips");
  const auto selected = event.animationClip
      ? (*event.animationClip < 0 ? std::nullopt : std::optional<size_t>(size_t(*event.animationClip)))
      : gltfAnimation_->selectedClip();
  if (!selected && (event.animationTime || event.animationPlaying.value_or(false)))
    throw std::invalid_argument("Capture must select a clip before seeking or playing");

  container::geometry::gltf::AnimationDirtySet dirty;
  const bool discontinuity = event.animationClip.has_value() || event.animationTime.has_value();
  if (discontinuity) {
    if (subs_.ddgiManager) subs_.ddgiManager->synchronizeAsync();
    auto& source = subs_.sceneManager->animationSource();
    if (event.animationClip) {
      dirty = selected ? gltfAnimation_->selectClip(source, *selected, event.animationTime.value_or(0.0))
                       : gltfAnimation_->clearClip(source);
    } else dirty = gltfAnimation_->seek(source, *event.animationTime);
  }
  pendingGltfAnimationDeltaSeconds_ = 0;
  if (event.animationPlaying) {
    if (*event.animationPlaying) gltfAnimation_->play();
    else gltfAnimation_->pause();
  }
  publishGltfAnimation(dirty, discontinuity);
}

void RendererFrontend::appendGltfAnimationTelemetry(nlohmann::json& json) const {
  using namespace container::geometry::gltf;
  auto& animation = json["gltfAnimation"];
  animation = {{"available",gltfAnimation_ != nullptr},
    {"clipCount",0},{"selectedClip",-1},{"time",0.0},{"playing",false},
    {"selectedCameraNode",gltfAnimationCameraNode_ ? static_cast<int>(*gltfAnimationCameraNode_) : -1}};
  auto matrix = [](const glm::mat4& value) {
    auto array = nlohmann::json::array();
    for (int column = 0; column < 4; ++column) for (int row = 0; row < 4; ++row) array.push_back(value[column][row]);
    return array;
  };
  if (gltfAnimationCamera_) {
    animation["cameraProjection"] = matrix(gltfAnimationCamera_->projection);
    animation["cameraNear"] = gltfAnimationCamera_->nearPlane;
    animation["cameraFar"] = gltfAnimationCamera_->farPlane;
    animation["orthographic"] = gltfAnimationCamera_->orthographic;
  }
  if (!gltfAnimation_ || !subs_.sceneManager) return;
  animation["clipCount"] = gltfAnimation_->library().clips.size();
  animation["selectedClip"] = gltfAnimation_->selectedClip() ? static_cast<int>(*gltfAnimation_->selectedClip()) : -1;
  animation["time"] = gltfAnimation_->time(); animation["playing"] = gltfAnimation_->playing();
  animation["speed"] = gltfAnimation_->speed(); animation["loop"] = gltfAnimation_->loop();
  animation["materialRevision"] = subs_.sceneManager->materialRevision();
  animation["diagnostics"] = gltfAnimation_->library().diagnostics.size();
  animation["targets"] = nlohmann::json::array();
  std::set<size_t> nodeIndices, materialIndices;
  if (gltfAnimation_->selectedClip()) {
    const auto pose = SampleAnimation(gltfAnimation_->library().clips[*gltfAnimation_->selectedClip()],gltfAnimation_->time());
    for (const auto& value : pose.values) {
      animation["targets"].push_back({{"kind",static_cast<int>(value.target.kind)},
        {"sourceIndex",value.target.index},{"property",value.target.property},
        {"component",value.target.element ? static_cast<int>(*value.target.element) : -1},{"value",value.value}});
      if (value.target.kind == AnimationTargetKind::NodeTranslation || value.target.kind == AnimationTargetKind::NodeRotation ||
          value.target.kind == AnimationTargetKind::NodeScale || value.target.kind == AnimationTargetKind::NodeWeights)
        nodeIndices.insert(value.target.index);
      if (value.target.kind == AnimationTargetKind::Material) materialIndices.insert(value.target.index);
    }
  }
  const auto& source = subs_.sceneManager->animationSource();
  animation["nodes"] = nlohmann::json::array();
  for (uint32_t i = 0; i < sceneGraph_.nodeCount(); ++i) {
    const auto* node = sceneGraph_.getNode(i);
    if (!node || node->sourceNodeIndex < 0 || !nodeIndices.contains(static_cast<size_t>(node->sourceNodeIndex))) continue;
    const auto& sourceNode = source.nodes[static_cast<size_t>(node->sourceNodeIndex)];
    animation["nodes"].push_back({{"sourceIndex",node->sourceNodeIndex},{"graphIndex",i},
      {"translation",sourceNode.translation},{"rotation",sourceNode.rotation},{"scale",sourceNode.scale},
      {"weights",sourceNode.weights},{"localMatrix",matrix(node->localTransform)},{"worldMatrix",matrix(node->worldTransform)}});
  }
  animation["pointLights"] = nlohmann::json::array();
  if (subs_.lightingManager) for (const auto& light : subs_.lightingManager->pointLightsSsbo())
    animation["pointLights"].push_back({{"position",{light.positionRadius.x,light.positionRadius.y,light.positionRadius.z}},
      {"color",{light.colorIntensity.x,light.colorIntensity.y,light.colorIntensity.z}},
      {"intensity",light.colorIntensity.w},{"range",light.positionRadius.w}});
  animation["materials"] = nlohmann::json::array();
  for (size_t sourceIndex : materialIndices) {
    for (uint32_t graphIndex : sceneGraph_.renderableNodes()) {
      const auto* node = sceneGraph_.getNode(graphIndex);
      if (!node || node->primitiveIndex >= subs_.sceneManager->primitiveRanges().size()) continue;
      const auto& primitive = subs_.sceneManager->primitiveRanges()[node->primitiveIndex];
      if (primitive.materialIndex < 0 || static_cast<size_t>(primitive.materialIndex) != sourceIndex) continue;
      const auto color = subs_.sceneManager->resolveMaterialColor(node->materialIndex);
      const auto factors = subs_.sceneManager->resolveMaterialMetallicRoughnessFactors(node->materialIndex);
      animation["materials"].push_back({{"sourceIndex",sourceIndex},{"gpuMaterialIndex",node->materialIndex},
        {"baseColor",{color.r,color.g,color.b,color.a}},{"metallic",factors.x},{"roughness",factors.y}});
      break;
    }
  }
}
} // namespace container::renderer
