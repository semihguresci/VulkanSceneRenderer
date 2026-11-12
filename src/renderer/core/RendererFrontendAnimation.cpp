#include <Container/renderer/core/RendererFrontend.h>

#include <Container/ecs/World.h>
#include <Container/geometry/GltfAnimationRuntime.h>
#include <Container/renderer/lighting/DdgiManager.h>
#include <Container/renderer/lighting/LightingManager.h>
#include <Container/renderer/scene/SceneController.h>
#include <Container/renderer/temporal/TemporalManager.h>
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
void RendererFrontend::resetGltfAnimation() {
  finishEditGestures();
  gltfAnimation_.reset();
  cameraProjectionInspectorCache_.reset();
  gltfAnimationCameraNode_.reset(); gltfAnimationCamera_.reset();
  gltfAnimationGraphIdentity_ = sceneGraph_.identity();
  pendingGltfAnimationDeltaSeconds_ = 0;
  resetGltfInteractivity();
  if (!subs_.sceneManager) return;
  container::ui::GltfAnimationControls controls;
  try {
    const auto& source = subs_.sceneManager->animationSource();
    gltfAnimation_ = std::make_unique<container::geometry::gltf::AnimationPlayer>(source);
    gltfAnimation_->setLoop(svc_.config.gltfAnimationLoop);
    gltfAnimation_->setSpeed(svc_.config.gltfAnimationSpeed);
    if (svc_.config.gltfCameraNode >= 0) {
      gltfAnimationCameraNode_ = static_cast<size_t>(svc_.config.gltfCameraNode);
      const auto extent = svc_.swapChainManager.extent();
      if (!container::geometry::gltf::EvaluateAnimatedCamera(source,sceneGraph_,*gltfAnimationCameraNode_,
          extent.height ? float(extent.width)/float(extent.height) : 1.0f))
        throw std::invalid_argument("configured glTF camera node has no active source camera binding");
    }
    if (svc_.config.gltfAnimationClip >= 0) {
      if (subs_.ddgiManager) subs_.ddgiManager->synchronizeAsync();
      auto& mutableSource = subs_.sceneManager->animationSource();
      auto dirty = gltfAnimation_->selectClip(mutableSource,static_cast<size_t>(svc_.config.gltfAnimationClip));
      dirty.merge(gltfAnimation_->seek(mutableSource,svc_.config.gltfAnimationTime));
      publishGltfAnimationChanges(dirty);
      if (svc_.config.gltfAnimationPlaying) gltfAnimation_->play();
      if (subs_.temporalManager) subs_.temporalManager->reset("configured glTF animation clip");
    }
    for (size_t i = 0; i < gltfAnimation_->library().clips.size(); ++i) {
      const auto& clip = gltfAnimation_->library().clips[i];
      controls.clips.push_back({clip.name.empty() ? "Clip " + std::to_string(i+1) : clip.name,clip.duration});
    }
    for (const auto& diagnostic : gltfAnimation_->library().diagnostics)
      controls.diagnostics.push_back("Clip " + std::to_string(diagnostic.clip+1) + ", channel " +
        std::to_string(diagnostic.channel+1) + ": " + diagnostic.message);
    controls.selectedClip = gltfAnimation_->selectedClip() ? static_cast<int>(*gltfAnimation_->selectedClip()) : -1;
    controls.time = gltfAnimation_->time(); controls.speed = gltfAnimation_->speed();
    controls.playing = gltfAnimation_->playing(); controls.loop = gltfAnimation_->loop();
    controls.cameraNode = gltfAnimationCameraNode_ ? static_cast<int>(*gltfAnimationCameraNode_) : -1;
  } catch (const std::exception& error) {
    controls.error = error.what();
    if (svc_.config.gltfAnimationClip >= 0 || svc_.config.gltfCameraNode >= 0) throw;
  }
  if (subs_.guiManager) subs_.guiManager->gltfAnimationControls() = std::move(controls);
  refreshGltfCameraChoices();
  updateCameraViewPoseDestination();
}
void RendererFrontend::refreshGltfCameraChoices() {
  if (subs_.sceneManager && subs_.guiManager)
    setGltfCameraChoices(sceneGraph_, subs_.sceneManager->animationSource(),
                         subs_.guiManager->gltfAnimationControls());
  gltfCameraChoicesDirty_ = false;
}

void RendererFrontend::setGltfCameraChoices(const container::scene::SceneGraph& graph,
    const tinygltf::Model& source, container::ui::GltfAnimationControls& controls) {
  std::vector<container::ui::GltfAnimationCameraChoice> choices;
  for(uint32_t graphIndex=0;graphIndex<graph.nodeCount();++graphIndex) {
    const auto& runtime=*graph.getNode(graphIndex);
    if(!graph.isNodeActive(graphIndex) || runtime.sourceNodeIndex<0) continue;
    const auto index=size_t(runtime.sourceNodeIndex);
    if(index>=source.nodes.size()) continue;
    const auto& node=source.nodes[index];
    if(node.camera<0 || size_t(node.camera)>=source.cameras.size()) continue;
    const auto& camera=source.cameras[size_t(node.camera)];
    std::string name=camera.name.empty()?"Camera "+std::to_string(node.camera+1):camera.name;
    if(!runtime.name.empty()) name+=" ("+runtime.name+")";
    choices.push_back({std::move(name),index});
  }
  controls.cameras = std::move(choices);
}
void RendererFrontend::publishGltfAnimationChanges(
    const container::geometry::gltf::AnimationDirtySet& dirty) {
  using namespace container::geometry::gltf;
  auto& source = subs_.sceneManager->animationSource();
  ApplyAnimatedNodeTransforms(source, dirty, sceneGraph_);
  if (!dirty.nodeFlags.empty()) {
    AnimationDirtySet materialAndCamera;
    materialAndCamera.materials = dirty.materials;
    materialAndCamera.cameras = dirty.cameras;
    subs_.sceneManager->publishAnimationChanges(materialAndCamera, sceneGraph_);
    // Rebuild the visible light bindings only after capturing overrides from
    // the previous visibility state, as for a direct editor flag change.
    publishSceneEdit();
    return;
  }
  subs_.sceneManager->publishAnimationChanges(dirty, sceneGraph_);
  if (subs_.lightingManager && (!dirty.nodes.empty() || !dirty.lights.empty()))
    subs_.lightingManager->updateLightingData();
  refreshSceneObjectData();
  syncSceneProviders();
}

void RendererFrontend::updateGltfAnimation() {
  using namespace container::geometry::gltf;
  if (gltfAnimationGraphIdentity_ != sceneGraph_.identity()) resetGltfAnimation();
  const double delta = std::exchange(pendingGltfAnimationDeltaSeconds_,0.0);
  if (!gltfAnimation_ || !subs_.sceneManager) return;
  auto& source = subs_.sceneManager->animationSource();
  auto* controls = subs_.guiManager ? &subs_.guiManager->gltfAnimationControls() : nullptr;
  AnimationDirtySet dirty;
  bool discontinuity = false;
  try {
    if (controls) {
      if(controls->requestedClip || controls->requestedSpeed || controls->requestedLoop || controls->requestedPlaying ||
          controls->requestedTime) finishEditGestures();
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
      if (auto camera = std::exchange(controls->requestedCameraNode,{})) {
        // Resolve/evaluate before publishing the selection. The common helper
        // resets temporal history once only after an accepted change.
        try {
          if(*camera< -1) throw std::invalid_argument("Invalid authored camera selector");
          (void)setGltfCameraNode(*camera<0?std::nullopt:std::optional<size_t>(size_t(*camera)));
          controls->cameraProjectionError.clear();
        } catch(const std::exception& error) {controls->cameraProjectionError=error.what();}
      }
    }
    dirty.merge(updateGltfInteractivity(delta, discontinuity));
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
    if (!dirty.empty()) {
      publishGltfAnimationChanges(dirty);
      // Rigid and deformed motion retain submitted transform/vertex history.
      // Timeline jumps and mutable shading invalidate incompatible color.
      if (subs_.temporalManager && dirty.nodeFlags.empty() && (discontinuity ||
          !dirty.materials.empty() || !dirty.lights.empty() || !dirty.cameras.empty()))
        subs_.temporalManager->reset("glTF animation seek, deformation or shading change");
    } else if (discontinuity && subs_.temporalManager) subs_.temporalManager->reset("glTF animation timeline or camera selection");
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
bool RendererFrontend::applyGltfAnimationCamera(uint32_t imageIndex) {
  gltfAnimationCamera_.reset();
  if (!gltfAnimationCameraNode_ || !subs_.sceneManager || imageIndex >= buffers_.cameras.size()) return false;
  const auto extent = svc_.swapChainManager.extent();
  if (!extent.width || !extent.height) return false;
  try {
    gltfAnimationCamera_ = container::geometry::gltf::EvaluateAnimatedCamera(
      subs_.sceneManager->animationSource(),sceneGraph_,*gltfAnimationCameraNode_,float(extent.width)/float(extent.height));
    if (!gltfAnimationCamera_) return false;
    buffers_.cameraData = gltfAnimationCamera_->data;
    if (subs_.sceneController)
      (void)subs_.sceneController->world().setActiveCamera(buffers_.cameraData,gltfAnimationCamera_->nearPlane,gltfAnimationCamera_->farPlane);
    SceneController::writeToBuffer(svc_.allocationManager,buffers_.cameras[imageIndex],&buffers_.cameraData,sizeof(buffers_.cameraData));
    return true;
  } catch (const std::exception& error) {
    if (subs_.guiManager) subs_.guiManager->gltfAnimationControls().error = error.what();
    return false;
  }
}
void RendererFrontend::appendGltfAnimationTelemetry(nlohmann::json& json) const {
  appendCameraProjectionTelemetry(json);
  appendCameraViewPoseTelemetry(json);
  appendLightEditTelemetry(json);
  using namespace container::geometry::gltf;
  auto& animation = json["gltfAnimation"];
  animation = {{"available",gltfAnimation_ != nullptr},
    {"selectedCameraNode",gltfAnimationCameraNode_ ? static_cast<int>(*gltfAnimationCameraNode_) : -1}};
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
  auto matrix = [](const glm::mat4& value) {
    auto array = nlohmann::json::array();
    for (int column = 0; column < 4; ++column) for (int row = 0; row < 4; ++row) array.push_back(value[column][row]);
    return array;
  };
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
  if (gltfAnimationCamera_) {
    animation["cameraProjection"] = matrix(gltfAnimationCamera_->projection);
    animation["cameraNear"] = gltfAnimationCamera_->nearPlane;
    animation["cameraFar"] = gltfAnimationCamera_->farPlane;
    animation["orthographic"] = gltfAnimationCamera_->orthographic;
  }
}
} // namespace container::renderer
