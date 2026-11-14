#include "Container/renderer/core/RendererFrontend.h"

#include "Container/app/AppConfig.h"
#include "Container/renderer/bim/BimManager.h"
#include "Container/renderer/lighting/DdgiManager.h"
#include "Container/renderer/lighting/LightingManager.h"
#include "Container/renderer/platform/VulkanContextInitializer.h"
#include "Container/renderer/scene/CameraController.h"
#include "Container/renderer/scene/SceneController.h"
#include "Container/renderer/scene/ScenePrimitives.h"
#include "Container/renderer/temporal/TemporalCapture.h"
#include "Container/utility/GuiManager.h"
#include "Container/utility/SceneManager.h"
#include "Container/utility/VulkanDevice.h"
#if CONTAINER_HAS_JOLT
#include "Container/physics/PhysicsScene.h"
#include "Container/physics/PhysicsElementBindings.h"
#endif

#include <glm/gtc/matrix_transform.hpp>
#include <glm/mat3x3.hpp>
#include <glm/matrix.hpp>
#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace container::renderer {

#if CONTAINER_HAS_JOLT
namespace {
void requireActivePrimaryPhysicsBodies(const container::scene::SceneGraph& graph,
                                      const container::physics::PhysicsScene& physics,
                                      const container::physics::PhysicsElementBindings* elements) {
  for (uint32_t node = 0; node < graph.nodeCount(); ++node) {
    if (physics.hasBody(node) && !graph.isNodeActive(node) &&
        (!elements || !elements->ownsNode(graph, node)))
      throw std::invalid_argument(
          "Physics is paused while bodies belong to another glTF scene; switch back or detach those bodies");
  }
}
} // namespace
#endif

bool RendererFrontend::applySceneNodeTransform(
    uint32_t node, const container::ui::TransformControls &controls) {
  if (!subs_.cameraController)
    return false;
  try {
    ensureSceneEditHistory();
    const auto gesture = transformDragSession_.active &&
                                 transformDragSession_.nodeIndex == node
        ? std::optional<uint64_t>{transformDragSession_.historyGesture}
        : subs_.guiManager ? subs_.guiManager->sceneEditGesture() : std::nullopt;
    const auto proposed = subs_.cameraController->previewNodeTransform(
        node, controls, sceneEdits_->transformGestureStart(node, gesture));
    if (!proposed || sceneGraph_.getNode(node)->localTransform == *proposed) return false;
    finishLightEditInput();
    finishMaterialGesture();
    return sceneEdits_->setTransform(node, *proposed, gesture);
  } catch (const std::exception &error) {
    if (subs_.guiManager)
      subs_.guiManager->setStatusMessage(
          "Cannot transform scene node " + std::to_string(node) + ": " + error.what());
    return false;
  }
}

void RendererFrontend::finishSceneTransformGesture(bool cancel) {
  transformDragSession_.active = false;
  if (!sceneEdits_) return;
  if (cancel) {
    try {
      sceneEdits_->cancelTransformGesture();
    } catch (const std::exception& error) {
      sceneEdits_->finishGesture();
      if (subs_.guiManager) subs_.guiManager->setStatusMessage(error.what());
    }
  } else {
    sceneEdits_->finishGesture();
  }
}

void RendererFrontend::validateSceneNodeTransform(uint32_t node,
                                                  const glm::mat4 &proposed) {
  requireEditableSceneNode(node);
#if CONTAINER_HAS_JOLT
  if (physicsScene_)
    physicsScene_->validateTransform(sceneGraph_, node, proposed);
  if (physicsElements_ && subs_.bimManager) {
    const auto updates = physicsElements_->previewTransforms(sceneGraph_, node, proposed);
    if (!updates.empty()) subs_.bimManager->validateObjectTransforms(updates);
  }
  if (physicsElements_ && physicsElements_->ownsNode(sceneGraph_, node)) {
    const auto *entry = sceneGraph_.getNode(node);
    const bool mirrored = glm::determinant(glm::dmat3{entry->localTransform}) < 0.0;
    const bool editedMirrored = glm::determinant(glm::dmat3{proposed}) < 0.0;
    if (mirrored != editedMirrored)
      throw std::invalid_argument(
          "Cannot change the reflection of BIM/USD physics entries: "
          "their triangle winding must remain unchanged");
  }
#else
  (void)proposed;
#endif
  // No-op targets never reach this callback. Finish validation before
  // pausing playback or publishing any graph/GPU data.
  pausePhysicsForTransformEdit(node);
}

void RendererFrontend::pausePhysicsForTransformEdit(uint32_t node) {
#if CONTAINER_HAS_JOLT
  if (!physicsScene_ || !physicsScene_->playing()) return;
  // A group edit also authors the world poses of its bound descendants.
  // Kinematic-only edits keep playing so authored motion can push contacts.
  if (physicsScene_->hasDynamicBodyInSubtree(sceneGraph_, node))
    physicsScene_->setPlaying(false);
#else
  (void)node;
#endif
}

void RendererFrontend::clearPhysicsBindings() {
#if CONTAINER_HAS_JOLT
  // A new graph can reuse old node indices. Retire bindings without restoring
  // transforms; PhysicsScene also checks the graph publication identity.
  if (physicsScene_) physicsScene_->clear();
  physicsElements_.reset();
#endif
  pendingPhysicsActions_.clear();
  physicsBimPublication_ = 0;
  physicsBimRootTranslation_ = glm::vec3{0.0f};
  pendingPhysicsDeltaSeconds_ = 0.0;
  physicsDemoNodes_ = {};
  physicsError_.clear();
  physicsJointFirstNode_.reset();
  physicsJointFirstBody_ = 0;
  physicsJointFirstGraph_ = 0;
  physicsJointFirstIsWorld_ = false;
  physicsSelectedJoint_ = 0;
}

void RendererFrontend::advancePhysics() {
  const double delta = std::exchange(pendingPhysicsDeltaSeconds_, 0.0);
#if CONTAINER_HAS_JOLT
  try {
    synchronizeBimPhysicsBindings();
    auto actions = std::exchange(pendingPhysicsActions_, {});
    for (const auto &action : actions) {
      if (!action.attach.empty()) {
        auto settings = action.bodySettings ? *action.bodySettings : container::physics::PhysicsBodySettings{};
        settings.motionType = action.attach == "static"
            ? container::physics::PhysicsMotionType::Static
            : action.attach == "kinematic" ? container::physics::PhysicsMotionType::Kinematic
                                             : container::physics::PhysicsMotionType::Dynamic;
        if (action.primaryNode) attachPhysicsBody(*action.primaryNode, settings);
        else attachBimPhysicsBody(action.bimObject, settings);
      }
      if (!action.action.empty())
        performPhysicsAction(action.action, action.primaryNode, action.bimObject);
      if (action.jointCreate) {
        if (!physicsScene_)
          throw std::invalid_argument("Attach physics bodies before connecting a joint");
        const auto resolve = [&](const container::temporal::CapturePhysicsJointBody &endpoint)
            -> std::optional<uint32_t> {
          if (endpoint.world) return std::nullopt;
          const auto node = endpoint.node ? endpoint.node
              : endpoint.bimObject && physicsElements_
                  ? physicsElements_->bodyNodeForObject(*endpoint.bimObject) : std::nullopt;
          if (!node || !physicsScene_->hasBody(*node))
            throw std::invalid_argument("Capture joint endpoint has no attached body");
          return node;
        };
        const auto first = resolve(action.jointCreate->bodyA);
        const auto second = resolve(action.jointCreate->bodyB);
        if (!second) throw std::invalid_argument("The second joint endpoint must be a body");
        physicsSelectedJoint_ = physicsScene_->createJoint(
            sceneGraph_, first, *second, action.jointCreate->settings);
      }
      if (!action.jointAction.empty()) {
        if (!physicsScene_) throw std::invalid_argument("No physics joint to remove");
        if (action.jointAction == "clear") {
          for (const auto &joint : physicsScene_->joints())
            (void)physicsScene_->destroyJoint(joint.id);
        } else if (!physicsScene_->destroyJoint(physicsSelectedJoint_)) {
          throw std::invalid_argument("The selected physics joint was retired");
        }
        physicsSelectedJoint_ = 0;
      }
      if (action.jointMotorVelocity) {
        const auto joint = physicsScene_ ? physicsScene_->jointState(physicsSelectedJoint_) : std::nullopt;
        if (!joint) throw std::invalid_argument("The selected physics joint was retired");
        auto motor = joint->state.settings.motor;
        motor.mode = container::physics::PhysicsJointMotorMode::Velocity;
        motor.targetVelocity = *action.jointMotorVelocity;
        if (!physicsScene_->setJointMotor(physicsSelectedJoint_, motor))
          throw std::invalid_argument("The selected physics joint has no motor");
      }
    }
    if (!actions.empty()) physicsError_.clear();
    if (!physicsScene_) return;
    physicsScene_->update(sceneGraph_, delta);
    publishBimPhysicsTransforms();
  } catch (const std::exception &error) {
    if (physicsScene_) physicsScene_->setPlaying(false);
    physicsError_ = error.what();
  }
#else
  (void)delta;
  if (!pendingPhysicsActions_.empty())
    throw std::runtime_error("Physics capture actions require ENABLE_PHYSICS=ON");
#endif
}

void RendererFrontend::synchronizeBimPhysicsBindings() {
#if CONTAINER_HAS_JOLT
  if (!physicsScene_ || !physicsElements_) return;
  const uint64_t publication = subs_.bimManager && subs_.bimManager->hasScene()
      ? subs_.bimManager->metadataRevision() : 0;
  physicsElements_->synchronizePublication(sceneGraph_, *physicsScene_, publication);
  const auto translation = publication ? subs_.bimManager->rootTranslation() : glm::vec3{0.0f};
  if (publication == physicsBimPublication_ && translation != physicsBimRootTranslation_)
    physicsElements_->translate(sceneGraph_, *physicsScene_, translation - physicsBimRootTranslation_);
  physicsBimPublication_ = publication;
  physicsBimRootTranslation_ = translation;
#endif
}

void RendererFrontend::publishBimPhysicsTransforms() {
#if CONTAINER_HAS_JOLT
  if (!physicsElements_ || !subs_.bimManager || !subs_.bimManager->hasScene()) return;
  auto updates = physicsElements_->collectTransforms(sceneGraph_);
  const auto &objects = subs_.bimManager->objectData();
  std::erase_if(updates, [&](const auto &update) {
    return update.objectIndex < objects.size() && objects[update.objectIndex].model == update.model;
  });
  if (updates.empty()) return;
  // Retire asynchronous readers before publishing the updated scene provider.
  if (subs_.ddgiManager) subs_.ddgiManager->synchronizeAsync();
  if (subs_.bimManager->setObjectTransforms(updates)) {
    selectionNavigationAnchor_ = {};
    clearHoveredMeshNode();
    syncSceneProviders();
  }
#endif
}

void RendererFrontend::attachBimPhysicsBody(
    uint32_t objectIndex, const container::physics::PhysicsBodySettings &settings) {
  finishEditGestures();
#if CONTAINER_HAS_JOLT
  if (!subs_.bimManager || !subs_.bimManager->hasScene())
    throw std::invalid_argument("Select a BIM or USD element to attach a body");
  const auto selected = subs_.bimManager->metadataForObject(objectIndex);
  if (!selected) throw std::invalid_argument("Selected BIM/USD element has no geometry metadata");
  if (!physicsScene_) physicsScene_ = std::make_unique<container::physics::PhysicsScene>();
  if (!physicsElements_) physicsElements_ = std::make_unique<container::physics::PhysicsElementBindings>();
  synchronizeBimPhysicsBindings();
  std::vector<container::physics::PhysicsElementFragment> fragments;
  const auto append = [&](uint32_t candidate) {
    const auto metadata = subs_.bimManager->metadataForObject(candidate);
    // GUID identity takes precedence over source IDs in the imported catalog.
    // Distinct products may share a source ID, so use its assigned product key.
    if (!metadata || metadata->productIdentityId != selected->productIdentityId) return;
    if (candidate >= subs_.bimManager->objectData().size() || !metadata->bounds.valid)
      throw std::invalid_argument("A BIM/USD product fragment has no finite collider bounds");
    fragments.push_back({candidate, subs_.bimManager->objectData()[candidate].model,
                         metadata->bounds.min, metadata->bounds.max});
  };
  if (!selected->guid.empty())
    for (const auto candidate : subs_.bimManager->objectIndicesForGuid(selected->guid)) append(candidate);
  else if (!selected->sourceId.empty())
    for (const auto candidate : subs_.bimManager->objectIndicesForSourceId(selected->sourceId)) append(candidate);
  else append(objectIndex);
  if (fragments.empty()) throw std::invalid_argument("Selected BIM/USD product has no collider fragments");
  std::string label = selected->displayName.empty() ? std::string(selected->type)
                                                  : std::string(selected->displayName);
  if (label.empty()) label = "BIM/USD element " + std::to_string(objectIndex);
  (void)physicsElements_->attachOrReplace(sceneGraph_, *physicsScene_,
      subs_.bimManager->metadataRevision(), selected->productIdentityId, fragments, settings, label);
#else
  (void)objectIndex; (void)settings;
  throw std::runtime_error("Physics support was disabled when this application was built");
#endif
}

void RendererFrontend::performPhysicsAction(std::string_view action,
    std::optional<uint32_t> primaryNode, uint32_t bimObject) {
  if (action == "demo") {
    createPhysicsDemo();
    return;
  }
  finishEditGestures();
#if CONTAINER_HAS_JOLT
  if (!physicsScene_) return;
  if (action == "play" || action == "step")
    requireActivePrimaryPhysicsBodies(sceneGraph_, *physicsScene_, physicsElements_.get());
  if (action == "play" || action == "pause") physicsScene_->setPlaying(action == "play");
  else if (action == "step") physicsScene_->singleStep(sceneGraph_);
  else if (action == "reset") physicsScene_->reset(sceneGraph_);
  else if (action == "detach") {
    if (primaryNode) (void)physicsScene_->detach(sceneGraph_, *primaryNode);
    else if (physicsElements_) {
      const auto restored = physicsElements_->detach(sceneGraph_, *physicsScene_, bimObject);
      if (subs_.bimManager && !restored.empty()) {
        if (subs_.ddgiManager) subs_.ddgiManager->synchronizeAsync();
        subs_.bimManager->setObjectTransforms(restored);
        selectionNavigationAnchor_ = {};
      }
    }
  } else if (action == "clear") {
    if (physicsElements_) {
      const auto restored = physicsElements_->clear(&sceneGraph_, *physicsScene_);
      if (subs_.bimManager && !restored.empty()) {
        if (subs_.ddgiManager) subs_.ddgiManager->synchronizeAsync();
        subs_.bimManager->setObjectTransforms(restored);
      }
    }
    physicsScene_->clear(&sceneGraph_);
    selectionNavigationAnchor_ = {};
  } else throw std::invalid_argument("Unknown physics action");
  publishBimPhysicsTransforms();
  if (action == "detach" || action == "clear") {
    clearHoveredMeshNode();
    syncSceneProviders();
  }
#else
  (void)action; (void)primaryNode; (void)bimObject;
  throw std::runtime_error("Physics support was disabled when this application was built");
#endif
}

void RendererFrontend::attachPhysicsBody(
    uint32_t nodeIndex, const container::physics::PhysicsBodySettings &settings) {
  requireEditableSceneNode(nodeIndex);
  finishEditGestures();
#if CONTAINER_HAS_JOLT
  const auto *node = sceneGraph_.getNode(nodeIndex);
  if (!node || !node->renderable || !sceneGraph_.isNodeActive(nodeIndex) || !subs_.sceneManager)
    throw std::invalid_argument("Select a primary scene mesh to attach a body");
  const auto &ranges = subs_.sceneManager->primitiveRanges();
  const auto &indices = subs_.sceneManager->indices();
  const auto &vertices = subs_.sceneManager->vertices();
  if (node->primitiveIndex >= ranges.size())
    throw std::invalid_argument("Selected mesh has no collider geometry");
  const auto &range = ranges[node->primitiveIndex];
  if (range.indexCount == 0 || uint64_t(range.firstIndex) + range.indexCount > indices.size())
    throw std::invalid_argument("Selected mesh has an invalid index range");
  glm::vec3 minimum{std::numeric_limits<float>::max()};
  glm::vec3 maximum{std::numeric_limits<float>::lowest()};
  for (uint64_t i = range.firstIndex; i < uint64_t(range.firstIndex) + range.indexCount; ++i) {
    if (indices[i] >= vertices.size())
      throw std::invalid_argument("Selected mesh has an invalid vertex index");
    const auto position = vertices[indices[i]].position;
    if (!std::isfinite(position.x) || !std::isfinite(position.y) || !std::isfinite(position.z))
      throw std::invalid_argument("Selected mesh contains a non-finite vertex");
    minimum = glm::min(minimum, position);
    maximum = glm::max(maximum, position);
  }
  if (!physicsScene_)
    physicsScene_ = std::make_unique<container::physics::PhysicsScene>();
  if (physicsScene_->hasBody(nodeIndex))
    (void)physicsScene_->replaceBox(sceneGraph_, nodeIndex, minimum, maximum, settings);
  else
    (void)physicsScene_->attachBox(sceneGraph_, nodeIndex, minimum, maximum, settings);
#else
  (void)nodeIndex;
  (void)settings;
  throw std::runtime_error("Physics support was disabled when this application was built");
#endif
}

void RendererFrontend::presentPhysicsControls() {
  container::ui::PhysicsPanelState state;
  state.error = physicsError_;
#if CONTAINER_HAS_JOLT
  state.available = true;
  synchronizeBimPhysicsBindings();
  const uint32_t selected = sceneState_.selectedMeshNode;
  const auto *node = sceneGraph_.getNode(selected);
  uint32_t bimObject = selectedBimObjectIndex_;
  if (bimObject == std::numeric_limits<uint32_t>::max() && physicsElements_) {
    const auto fragments = physicsElements_->objectsForNode(selected);
    if (!fragments.empty()) bimObject = fragments.front();
  }
  const bool auxiliary = subs_.bimManager && subs_.bimManager->hasScene() &&
                         subs_.bimManager->metadataForObject(bimObject).has_value();
  state.selectionAvailable = auxiliary || (node && node->renderable);
  std::optional<uint32_t> bodyNode = auxiliary
      ? (physicsElements_ ? physicsElements_->bodyNodeForObject(bimObject) : std::nullopt)
      : std::optional<uint32_t>{selected};
  if (auxiliary) {
    const auto metadata = *subs_.bimManager->metadataForObject(bimObject);
    state.selectionLabel = metadata.displayName.empty() ? std::string(metadata.type)
                                                       : std::string(metadata.displayName);
  } else if (node) state.selectionLabel = node->name;
  if (physicsScene_) {
    const auto stats = physicsScene_->statistics();
    state.bodyCount = stats.bodyCount;
    state.activeBodyCount = stats.activeBodyCount;
    state.totalSteps = stats.totalSteps;
    state.playing = physicsScene_->playing();
    state.selectedHasBody = bodyNode && physicsScene_->hasBody(*bodyNode);
    if (bodyNode) {
      state.selectedMotion = physicsScene_->motionType(*bodyNode);
      state.selectedBodyId = physicsScene_->bodyId(*bodyNode).value_or(0);
      state.selectedBodySettings = physicsScene_->bodySettings(*bodyNode);
    }
  }
  const auto action = [this](std::function<void()> operation) {
    try {
      operation();
      physicsError_.clear();
    } catch (const std::exception &error) {
      physicsError_ = error.what();
      if (physicsScene_) physicsScene_->setPlaying(false);
    }
  };
  state.attachSelected = [this, action, selected, auxiliary, bimObject](const auto &settings) {
    action([&] {
      if (auxiliary) attachBimPhysicsBody(bimObject, settings);
      else attachPhysicsBody(selected, settings);
    });
  };
  state.detachSelected = [this, action, selected, auxiliary, bimObject] {
    action([&] { performPhysicsAction("detach", auxiliary ? std::nullopt : std::optional{selected}, bimObject); });
  };
  state.setPlaying = [this, action](bool playing) {
    action([&] { performPhysicsAction(playing ? "play" : "pause", std::nullopt, 0); });
  };
  state.singleStep = [this, action] {
    action([&] { performPhysicsAction("step", std::nullopt, 0); });
  };
  state.reset = [this, action] {
    action([&] { performPhysicsAction("reset", std::nullopt, 0); });
  };
  state.clear = [this, action] {
    action([&] { performPhysicsAction("clear", std::nullopt, 0); });
  };
  state.createDemo = [this, action] { action([&] { createPhysicsDemo(); }); };
  if (physicsJointFirstNode_ &&
      (physicsJointFirstGraph_ != sceneGraph_.identity() || !physicsScene_ ||
       physicsScene_->bodyId(*physicsJointFirstNode_) != physicsJointFirstBody_)) {
    physicsJointFirstNode_.reset();
    physicsJointFirstBody_ = 0;
  }
  state.jointFirstChosen = physicsJointFirstIsWorld_ || physicsJointFirstNode_.has_value();
  if (state.jointFirstChosen) {
    state.jointFirstBodyLabel = physicsJointFirstIsWorld_ ? "World"
        : sceneGraph_.getNode(*physicsJointFirstNode_)->name;
  }
  const bool distinct = bodyNode &&
      (physicsJointFirstIsWorld_ || (physicsJointFirstNode_ && *bodyNode != *physicsJointFirstNode_));
  const bool dynamicPair = physicsScene_ && bodyNode &&
      (physicsScene_->motionType(*bodyNode) == container::physics::PhysicsMotionType::Dynamic ||
       (physicsJointFirstNode_ && physicsScene_->motionType(*physicsJointFirstNode_) ==
                                         container::physics::PhysicsMotionType::Dynamic));
  state.canConnectJoint = state.jointFirstChosen && state.selectedHasBody && distinct && dynamicPair;
  if (!state.jointFirstChosen) state.jointConnectionHint = "Choose a first body or the world anchor.";
  else if (!state.selectedHasBody) state.jointConnectionHint = "Select another object with an attached body.";
  else if (!distinct) state.jointConnectionHint = "Choose two different bodies.";
  else if (!dynamicPair) state.jointConnectionHint = "At least one connected body must be dynamic.";
  if (physicsScene_ && bodyNode) {
    const auto secondPose = physicsScene_->bodyPose(*bodyNode);
    if (secondPose) {
      state.suggestedJointAnchor = secondPose->position;
      if (physicsJointFirstNode_)
        if (const auto firstPose = physicsScene_->bodyPose(*physicsJointFirstNode_))
          state.suggestedJointAnchor = (firstPose->position + secondPose->position) * 0.5;
    }
  }
  state.chooseFirstJointBody = [this, action, bodyNode] {
    action([&] {
      if (!bodyNode || !physicsScene_ || !physicsScene_->hasBody(*bodyNode))
        throw std::invalid_argument("Select a body before choosing the first joint endpoint");
      physicsJointFirstNode_ = *bodyNode;
      physicsJointFirstBody_ = *physicsScene_->bodyId(*bodyNode);
      physicsJointFirstGraph_ = sceneGraph_.identity();
      physicsJointFirstIsWorld_ = false;
    });
  };
  state.chooseWorldJointBody = [this, action] {
    action([&] {
      physicsJointFirstNode_.reset();
      physicsJointFirstBody_ = 0;
      physicsJointFirstGraph_ = sceneGraph_.identity();
      physicsJointFirstIsWorld_ = true;
    });
  };
  state.clearFirstJointBody = [this] {
    physicsJointFirstNode_.reset();
    physicsJointFirstIsWorld_ = false;
    physicsJointFirstBody_ = 0;
  };
  state.connectJoint = [this, action, bodyNode](const container::physics::PhysicsJointSettings &settings) {
    action([&] {
      if (!physicsScene_ || !bodyNode || (!physicsJointFirstNode_ && !physicsJointFirstIsWorld_))
        throw std::invalid_argument("Choose two attached joint bodies or a world anchor");
      physicsSelectedJoint_ = physicsScene_->createJoint(sceneGraph_,
          physicsJointFirstIsWorld_ ? std::nullopt : physicsJointFirstNode_, *bodyNode, settings);
    });
  };
  state.removeJoint = [this, action](container::physics::PhysicsJointId id) {
    action([&] {
      if (!physicsScene_ || !physicsScene_->destroyJoint(id))
        throw std::invalid_argument("The physics joint was already retired");
      if (physicsSelectedJoint_ == id) physicsSelectedJoint_ = 0;
    });
  };
  state.setJointMotor = [this, action](container::physics::PhysicsJointId id,
      const container::physics::PhysicsJointMotorSettings &motor) {
    action([&] {
      if (!physicsScene_ || !physicsScene_->setJointMotor(id, motor))
        throw std::invalid_argument("The physics joint has no editable motor");
    });
  };
  if (physicsScene_) {
    for (const auto &joint : physicsScene_->joints()) {
      const auto *first = joint.nodeA ? sceneGraph_.getNode(*joint.nodeA) : nullptr;
      const auto *second = sceneGraph_.getNode(joint.nodeB);
      if ((joint.nodeA && !first) || !second)
        continue;
      const auto firstName = first ? first->name : "World";
      const auto &secondName = second->name;
      state.joints.push_back({joint.id, firstName + " -> " + secondName,
                              joint.state.type, joint.state.coordinate});
    }
  }
#endif
  subs_.guiManager->setPhysicsPanelState(std::move(state));
}

void RendererFrontend::createPhysicsDemo() {
#if CONTAINER_HAS_JOLT
  if (!subs_.sceneController || !subs_.sceneManager)
    throw std::runtime_error("The scene is not ready for a physics demo");
  if (physicsScene_)
    requireActivePrimaryPhysicsBodies(sceneGraph_, *physicsScene_, physicsElements_.get());
  finishEditGestures();
  constexpr uint32_t demoBodyCount = PhysicsDemoNodes::kBodyCount;
  const bool reuse = physicsDemoNodes_.reusable(sceneGraph_) &&
      std::ranges::all_of(physicsDemoNodes_.bodies,
          [this](uint32_t node) { return sceneGraph_.isNodeActive(node); });
  const auto demoNodes = physicsDemoNodes_.bodies;
  if (!reuse && sceneGraph_.renderableNodes().size() + demoBodyCount > svc_.config.maxSceneObjects)
    throw std::runtime_error("Not enough scene object capacity for the physics demo");
  // Runtime geometry replaces shared buffers. Finish their graphics and async
  // compute readers before using the existing primitive upload path.
  if (subs_.ddgiManager) subs_.ddgiManager->synchronizeAsync();
  svc_.ctx.deviceWrapper->raii().waitIdle();
  float offsetX = 12.0f;
  if (!reuse) {
    // Existing demos are separate roots. Include all rendered instances so an
    // edited demo can remain intact without overlapping the replacement.
    for (const auto &object : subs_.sceneController->objectData()) {
      const auto sphere = object.boundingSphere;
      const float nextOffset = sphere.x + std::max(sphere.w, 0.0f) + 12.0f;
      if (std::isfinite(nextOffset))
        offsetX = std::max(offsetX, nextOffset);
    }
  }
  uint32_t group = reuse ? physicsDemoNodes_.root : sceneGraph_.createNode(
      glm::translate(glm::mat4{1.0f}, {offsetX, 0.0f, 0.0f}), 0, false,
      container::scene::SceneGraph::kInvalidNode, "Physics demo");
  if (!reuse)
    physicsDemoNodes_ = {.graphIdentity = sceneGraph_.identity(), .root = group};
  for (uint32_t i = 0; i < demoBodyCount; ++i) {
    const glm::vec3 position = i == 0 ? glm::vec3{0.0f, -0.25f, 0.0f}
                                    : glm::vec3{(float(i) - 2.0f) * 1.5f, 2.0f + i, 0.0f};
    const glm::vec3 scale = i == 0 ? glm::vec3{12.0f, 0.5f, 12.0f} : glm::vec3{1.0f};
    const auto transform = glm::translate(glm::mat4{1.0f}, position) *
                           glm::scale(glm::mat4{1.0f}, scale);
    const uint32_t node = reuse ? demoNodes[i] :
        subs_.sceneController->addScenePrimitive(ScenePrimitiveKind::Cube, transform, group);
    if (node == container::scene::SceneGraph::kInvalidNode)
      throw std::runtime_error("Failed to create physics demo geometry");
    physicsDemoNodes_.bodies[i] = node;
    sceneGraph_.setName(node, i == 0 ? "Physics floor" : "Physics box " + std::to_string(i));
    if (reuse) {
      if (physicsScene_) (void)physicsScene_->detach(sceneGraph_, node);
      sceneGraph_.setLocalTransform(node, transform);
      sceneGraph_.updateWorldTransforms();
    }
    container::physics::PhysicsBodySettings settings;
    settings.motionType = i == 0 ? container::physics::PhysicsMotionType::Static
                                : container::physics::PhysicsMotionType::Dynamic;
    settings.restitution = 0.1f;
    attachPhysicsBody(node, settings);
  }
  physicsScene_->setPlaying(true);
  syncSceneStateFromController();
  refreshSceneObjectData();
  syncSceneProviders();
  // The default Cornell fixture deliberately disables sunlight. Give the
  // explicit demo a usable light unless the caller specified its intensity.
  if (subs_.lightingManager && !sceneLightingOverrides_.directionalIntensity) {
    auto lighting = subs_.lightingManager->lightingSettings();
    lighting.directionalIntensity = std::max(lighting.directionalIntensity, 2.0f);
    subs_.lightingManager->setLightingSettings(lighting);
    // A UI callback runs after the frame's lighting snapshot was published.
    // Keep its later writeback from restoring the scene's previous intensity.
    if (subs_.guiManager) subs_.guiManager->setLightingSettings(lighting);
  }
  if (subs_.cameraController) {
    subs_.cameraController->selectMeshNode(group, sceneState_.selectedMeshNode);
    if (!svc_.config.hasCameraOverride) subs_.cameraController->frameNodeOrScene(group);
  }
  selectedBimObjectIndex_ = std::numeric_limits<uint32_t>::max();
  selectionNavigationAnchor_ = {};
  clearHoveredMeshNode();
#else
  throw std::runtime_error("--physics-demo requires a build with ENABLE_PHYSICS=ON");
#endif
}

void RendererFrontend::appendPhysicsTelemetry(nlohmann::json &json) const {
#if CONTAINER_HAS_JOLT
  json["physics"] = {{"available", true}, {"initialized", bool(physicsScene_)},
                      {"fixedStepSeconds", container::physics::PhysicsScene::kFixedStepSeconds}};
  if (!physicsScene_) return;
  const auto stats = physicsScene_->statistics();
  auto &data = json["physics"];
  data["playing"] = physicsScene_->playing();
  data["bodyCount"] = stats.bodyCount;
  data["activeBodyCount"] = stats.activeBodyCount;
  data["totalSteps"] = stats.totalSteps;
  data["substeps"] = stats.substeps;
  data["droppedSeconds"] = stats.droppedSeconds;
  data["error"] = physicsError_;
  data["jointCount"] = stats.jointCount;
  data["joints"] = nlohmann::json::array();
  for (const auto &joint : physicsScene_->joints()) {
    const auto &settings = joint.state.settings;
    const auto type = joint.state.type == container::physics::PhysicsJointType::Fixed ? "fixed"
        : joint.state.type == container::physics::PhysicsJointType::Hinge ? "hinge" : "slider";
    const auto mode = settings.motor.mode == container::physics::PhysicsJointMotorMode::Off ? "off"
        : settings.motor.mode == container::physics::PhysicsJointMotorMode::Velocity ? "velocity" : "position";
    data["joints"].push_back({{"id", joint.id}, {"type", type},
        {"nodeA", joint.nodeA ? nlohmann::json(*joint.nodeA) : nlohmann::json(nullptr)},
        {"nodeB", joint.nodeB}, {"coordinate", joint.state.coordinate},
        {"settings", {{"anchor", {settings.anchor.x, settings.anchor.y, settings.anchor.z}},
            {"axis", {settings.axis.x, settings.axis.y, settings.axis.z}},
            {"limitsEnabled", settings.limitsEnabled}, {"lowerLimit", settings.lowerLimit},
            {"upperLimit", settings.upperLimit}, {"collideConnected", settings.collideConnected},
            {"motor", {{"mode", mode}, {"targetVelocity", settings.motor.targetVelocity},
                {"targetPosition", settings.motor.targetPosition},
                {"maxForceOrTorque", settings.motor.maxForceOrTorque},
                {"frequencyHz", settings.motor.frequencyHz}, {"dampingRatio", settings.motor.dampingRatio}}}}}});
  }
  data["bodies"] = nlohmann::json::array();
  data["auxiliaryElements"] = nlohmann::json::array();
  std::unordered_map<uint32_t, const container::gpu::ObjectData *> rendered;
  if (subs_.sceneController) {
    const auto &objects = subs_.sceneController->objectData();
    for (uint32_t object = 0; object < objects.size(); ++object)
      rendered.emplace(subs_.sceneController->nodeIndexForObject(object), &objects[object]);
  }
  for (uint32_t node = 0; node < sceneGraph_.nodeCount(); ++node) {
    if (const auto pose = physicsScene_->bodyPose(node)) {
      const auto *entry = sceneGraph_.getNode(node);
      data["bodies"].push_back({{"node", node}, {"id", *physicsScene_->bodyId(node)},
          {"name", entry->name},
          {"motion", uint32_t(*physicsScene_->motionType(node))},
          {"position", {pose->position.x, pose->position.y, pose->position.z}},
          {"rotation", {pose->rotation.x, pose->rotation.y, pose->rotation.z, pose->rotation.w}}});
      auto &body = data["bodies"].back();
      if (const auto settings = physicsScene_->bodySettings(node)) {
        body["settings"] = {{"mass", settings->mass}, {"friction", settings->friction},
            {"restitution", settings->restitution}, {"gravityFactor", settings->gravityFactor},
            {"useCCD", settings->useCCD},
            {"linearVelocity", {settings->linearVelocity.x, settings->linearVelocity.y, settings->linearVelocity.z}},
            {"angularVelocity", {settings->angularVelocity.x, settings->angularVelocity.y, settings->angularVelocity.z}}};
      }
      body["sceneWorldMatrix"] = nlohmann::json::array();
      body["renderWorldMatrix"] = nlohmann::json::array();
      const auto object = rendered.find(node);
      for (int column = 0; column < 4; ++column)
        for (int row = 0; row < 4; ++row) {
          body["sceneWorldMatrix"].push_back(entry->worldTransform[column][row]);
          if (object != rendered.end())
            body["renderWorldMatrix"].push_back(object->second->model[column][row]);
        }
      if (physicsElements_ && subs_.bimManager) {
        const auto fragments = physicsElements_->objectsForNode(node);
        if (!fragments.empty()) {
          auto element = nlohmann::json{{"node", node},
              {"productId", *physicsElements_->productForObject(fragments.front())},
              {"objects", nlohmann::json::array()}};
          for (const uint32_t objectIndex : fragments) {
            const auto metadata = subs_.bimManager->metadataForObject(objectIndex);
            if (!metadata || objectIndex >= subs_.bimManager->objectData().size()) continue;
            const auto &objectData = subs_.bimManager->objectData()[objectIndex];
            nlohmann::json fragment{{"objectIndex", objectIndex},
                {"guid", std::string(metadata->guid)}, {"sourceId", std::string(metadata->sourceId)},
                {"boundsMin", {metadata->bounds.min.x, metadata->bounds.min.y, metadata->bounds.min.z}},
                {"boundsMax", {metadata->bounds.max.x, metadata->bounds.max.y, metadata->bounds.max.z}},
                {"model", nlohmann::json::array()}, {"normalMatrix", nlohmann::json::array()}};
            for (int column = 0; column < 4; ++column)
              for (int row = 0; row < 4; ++row)
                fragment["model"].push_back(objectData.model[column][row]);
            for (const auto normal : {objectData.normalMatrix0, objectData.normalMatrix1, objectData.normalMatrix2})
              for (int row = 0; row < 4; ++row) fragment["normalMatrix"].push_back(normal[row]);
            element["objects"].push_back(std::move(fragment));
          }
          data["auxiliaryElements"].push_back(std::move(element));
        }
      }
    }
  }
#else
  json["physics"] = {{"available", false}, {"initialized", false}};
#endif
}
} // namespace container::renderer
