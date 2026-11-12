#pragma once

#include "Container/renderer/temporal/UpscaleMode.h"

#include "Container/physics/PhysicsWorld.h"
#include "Container/geometry/GltfMergePlacement.h"
#include "Container/utility/SceneNodeAuthoringPolicy.h"
#include "Container/utility/GltfMaterialEditRequest.h"
#include "Container/utility/GltfMaterialBindingRequest.h"
#include "Container/utility/ResourceCatalogControls.h"
#include "Container/geometry/GltfCameraAuthoring.h"
#include "Container/renderer/lighting/DdgiSettings.h"
#include "Container/renderer/lighting/LightEditTypes.h"
#include "Container/renderer/lighting/SkySettings.h"
#include "Container/renderer/pathtracing/ReferenceOidnSettings.h"
#include "Container/renderer/pathtracing/ReferencePathDisplayMode.h"

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace container::temporal {

struct CaptureCamera {
  glm::vec3 position{}, target{};
};
struct CapturePhysicsJointBody {
  std::optional<uint32_t> node, bimObject;
  bool world{false};
};
struct CapturePhysicsJointCreate {
  CapturePhysicsJointBody bodyA, bodyB;
  container::physics::PhysicsJointSettings settings{};
};
struct CaptureNodeFlags {
  int sourceNode{};
  std::optional<bool> visible, selectable, hoverable;
};
struct CaptureMergeRequest {
  std::string path;
  std::string parentName{"Imported scene"};
  std::optional<int> incomingScene;
  // glTF-native column-major affine matrix; preserve authored input precision.
  std::optional<std::array<double, 16>> placementMatrix;
};
struct CaptureNodeAuthoring {
  std::string action;
  std::optional<uint32_t> graphNode;
  std::optional<int> sourceNode;
  // Rename/create require a label; delete forbids it; duplicate may omit it.
  std::string name;
};
struct CaptureNodeReference {
  std::optional<uint32_t> graphNode;
  std::optional<int> sourceNode;
};
struct CaptureNodeReparent {
  CaptureNodeReference node, parent;
};
struct CaptureNodeTransform {
  CaptureNodeReference node;
  glm::mat4 matrix{1.0f};
};
struct CaptureAddPrimitive {
  std::string kind, name;
  std::optional<CaptureNodeReference> parent;
};
using CaptureMaterialEdit = container::material::GltfMaterialEditRequest;
using CaptureMaterialBinding = container::material::GltfMaterialBindingRequest;
using CaptureMaterialTextureEdit = container::material::GltfMaterialTextureEditRequest;
struct CaptureMaterialRetirement {
  uint32_t runtimeMaterial{};
  int32_t sourceMaterial{-1};
  std::optional<uint64_t> expectedAuthoredRevision;
};
struct CaptureResourceRetirement {
  container::ui::ResourceCatalogDomain domain{};
  container::ui::ResourceCatalogKind kind{};
  uint32_t row{};
  std::optional<uint64_t> expectedResourceRevision, expectedIncarnation;
};
struct CaptureCameraProjection {
  int32_t sourceNode{};
  std::optional<uint64_t> expectedCameraRevision;
  container::geometry::gltf::GltfCameraProjectionPatch patch;
};
struct CaptureCameraSelection {
  std::optional<int32_t> sourceNode;
  std::optional<std::string> nodeName;
  bool viewport{};
};
struct CaptureCameraPose {
  int32_t sourceNode{};
  std::optional<uint64_t> expectedCameraRevision;
};
struct CaptureLightTarget {
  container::renderer::EditableLightType type{};
  container::renderer::EditableLightSource source{};
  std::optional<int32_t> sourceNode;
  std::string nodeName;
  std::optional<uint32_t> index;
};
struct CaptureLightGizmo {
  enum class Tool { Translate,Rotate,Scale };
  enum class Space { World,Local };
  enum class Axis { Free,X,Y,Z };
  Tool tool{};
  glm::vec3 amount{};
  Space space{Space::World};
  Axis axis{Axis::Free};
  bool snap{};
};
struct CaptureLightEdit {
  CaptureLightTarget target;
  container::renderer::LightEditPhase phase{container::renderer::LightEditPhase::Commit};
  uint64_t gesture{};
  std::optional<uint64_t> expectedLightRevision;
  std::optional<container::renderer::LightEditPatch> values;
  std::optional<CaptureLightGizmo> gizmo;
};
struct CaptureLightAdd {
  container::renderer::EditableLightType type{};
  std::optional<uint64_t> expectedLightRevision;
};
struct CaptureComfyUi {
  enum class Action { GenerateEnvironment, RestyleViewport, ApplyEnvironment, Cancel };
  Action action{};
  std::string prompt;
  uint64_t seed{1};
  bool wait{};
};
struct CaptureEvent {
  uint32_t frame{0};
  bool reset{false}, skip{false};
  bool acquireOutOfDate{false}, presentSuboptimal{false};
  bool guiUnload{false};
  bool guiWaitForLoad{true};
  std::optional<bool> taa, orthographic, objectVisible, minimized;
  std::optional<container::renderer::UpscaleSelection> upscaling;
  std::optional<bool> rayShadowDenoise;
  std::optional<bool> ddgiPaused, ddgiEnabled;
  std::optional<float> ddgiIntensity;
  std::optional<uint32_t> ddgiFitProbes;
  std::optional<uint32_t> rayShadowSamples;
  std::optional<uint32_t> areaLightSamples;
  std::optional<uint32_t> samples;
  std::optional<uint32_t> bimHiddenObject;
  std::optional<uint32_t> bimSelectedObject;
  std::optional<uint32_t> physicsNode;
  std::optional<container::physics::PhysicsBodySettings> physicsSettings;
  std::optional<CapturePhysicsJointCreate> physicsJointCreate;
  std::optional<float> physicsJointMotorVelocity;
  std::optional<int> bimLodBias;
  std::optional<glm::vec4> sectionPlane;
  std::optional<glm::vec3> areaLightPosition;
  std::optional<float> exposure;
  std::optional<glm::uvec2> resize;
  std::optional<glm::uvec2> depthProbe;
  std::string technique{}, reload{}, guiReload{}, guiSampleModel{};
  std::string environment{};
  bool environmentAsync{};
  bool reloadShaders{};
  bool referenceAutofocus{};
  std::optional<container::renderer::ReferenceOidnQuality> referenceOidn;
  std::optional<bool> referenceOptix;
  std::optional<container::renderer::UpscaleMode> referenceDlss;
  std::optional<container::renderer::ReferencePathDisplayMode> referenceDisplay;
  std::optional<CaptureComfyUi> comfyUi;
  std::optional<bool> gpuTransforms;
  std::optional<container::renderer::SkyMode> proceduralSky;
  std::optional<float> environmentYaw;
  std::string rayShadows{};
  std::string areaLighting{};
  // Omitted/empty values are inert. Selection is resolved at event execution,
  // after any requested model replacement has finished.
  std::string physicsAttach{}, physicsAction{};
  std::string physicsJointAction{};
  std::optional<int> materialVariant;
  std::optional<int> gltfScene;
  std::optional<CaptureNodeFlags> nodeFlags;
  std::optional<CaptureMergeRequest> merge;
  std::optional<CaptureNodeAuthoring> nodeAuthoring;
  std::optional<CaptureNodeReparent> nodeReparent;
  std::optional<CaptureNodeTransform> nodeTransform;
  std::optional<CaptureAddPrimitive> addPrimitive;
  std::optional<CaptureMaterialEdit> materialEdit;
  std::optional<CaptureMaterialBinding> materialBinding;
  std::optional<CaptureMaterialTextureEdit> materialTextureEdit;
  std::optional<CaptureMaterialRetirement> materialRetirement;
  std::optional<CaptureResourceRetirement> resourceRetirement;
  std::optional<CaptureCameraProjection> cameraProjection;
  std::optional<CaptureCameraSelection> cameraSelection;
  std::optional<CaptureCameraPose> cameraPose;
  bool expectCameraProjectionFailure{}, expectCameraSelectionFailure{}, expectCameraPoseFailure{};
  std::optional<CaptureLightEdit> lightEdit;
  std::optional<CaptureLightTarget> lightSelection;
  std::optional<CaptureLightAdd> lightAdd;
  bool expectLightEditFailure{};
  // Capture automation only: require this backend texture action to reject and
  // continue rendering so its unchanged state can be observed afterwards.
  bool expectMaterialTextureFailure{false};
  // Uses the ordinary retirement backend and requires rejection; captures
  // before/after state instead of terminating on that expected error.
  bool expectMaterialRetirementFailure{false};
  bool expectResourceRetirementFailure{false};
  std::optional<bool> animationPlaying;
  std::optional<int> animationClip;
  std::optional<double> animationTime;
  std::optional<bool> interactivityPlaying;
  bool interactivityReset{};
  std::optional<std::string> interactivityEvent;
  std::string sceneHistory;
};
struct CaptureSample {
  std::optional<CaptureCamera> camera;
  std::optional<glm::vec3> objectTranslation, objectScale, bimTranslation;
  uint32_t objectNode{0};
  std::string objectName{};
  CaptureEvent event{};
};

// Pure, deterministic keyframe evaluation. Capture scripts never use elapsed
// wall time, and an event applies exactly once at its named simulation frame.
class CaptureSequence {
public:
  explicit CaptureSequence(const nlohmann::json &json) {
    if (json.at("schemaVersion") != 1)
      throw std::invalid_argument("Unsupported temporal capture schema");
    frameCount = json.at("frames").get<uint32_t>();
    if (frameCount == 0 || frameCount > 10000)
      throw std::invalid_argument("Capture frames must be in [1,10000]");
    sampleFrames =
        json.value("sampleFrames", std::vector<uint32_t>{frameCount});
    for (auto frame : sampleFrames)
      checkFrame(frame);
    if (json.contains("telemetryFrames")) {
      const auto &frames = json.at("telemetryFrames");
      if (!frames.is_array() || frames.size() > frameCount)
        throw std::invalid_argument("Invalid capture telemetry frame list");
      telemetryFrames_.reserve(frames.size());
      for (const auto &value : frames) {
        if (!value.is_number_integer())
          throw std::invalid_argument("Capture telemetry frames must be integers");
        const auto frame = value.get<uint64_t>();
        if (frame == 0 || frame > frameCount)
          throw std::invalid_argument("Capture telemetry frame is out of range");
        telemetryFrames_.push_back(static_cast<uint32_t>(frame));
      }
    }
    std::ranges::sort(telemetryFrames_);
    if (std::ranges::adjacent_find(telemetryFrames_) != telemetryFrames_.end())
      throw std::invalid_argument("Capture telemetry frames must be distinct");
    if (json.contains("camera")) {
      for (const auto &key : json.at("camera")) {
        const uint32_t frame = key.at("frame").get<uint32_t>();
        checkFrame(frame);
        CaptureCamera pose{vec3(key.at("position")), vec3(key.at("target"))};
        if (glm::length(pose.position - pose.target) < 1e-4f)
          throw std::invalid_argument("Camera position equals target");
        cameras_.push_back({frame, pose});
      }
      validateOrder(cameras_);
    }
    objectNode_ = json.value("objectNode", 0u);
    objectName_ = json.value("objectName", std::string{});
    readTranslations(json, "objectTranslation", objects_);
    readScales(json);
    readTranslations(json, "bimTranslation", bim_);
    for (const auto &value : json.value("events", nlohmann::json::array())) {
      CaptureEvent event;
      event.frame = value.at("frame").get<uint32_t>();
      checkFrame(event.frame);
      event.reset = value.value("reset", false);
      event.skip = value.value("skip", false);
      event.acquireOutOfDate = value.value("acquireOutOfDate", false);
      event.presentSuboptimal = value.value("presentSuboptimal", false);
      if (value.contains("merge"))
        event.merge = mergeRequest(value.at("merge"));
      if (value.contains("nodeAuthoring"))
        event.nodeAuthoring = nodeAuthoringRequest(value.at("nodeAuthoring"));
      if (value.contains("nodeReparent"))
        event.nodeReparent = nodeReparentRequest(value.at("nodeReparent"));
      if (value.contains("nodeTransform"))
        event.nodeTransform = nodeTransformRequest(value.at("nodeTransform"));
      if (value.contains("addPrimitive"))
        event.addPrimitive = addPrimitiveRequest(value.at("addPrimitive"));
      if (value.contains("materialEdit"))
        event.materialEdit = materialEditRequest(value.at("materialEdit"));
      if (value.contains("materialBinding"))
        event.materialBinding = materialBindingRequest(value.at("materialBinding"));
      if (value.contains("materialTextureEdit"))
        event.materialTextureEdit = materialTextureEditRequest(value.at("materialTextureEdit"));
      if (value.contains("materialRetirement"))
        event.materialRetirement = materialRetirementRequest(value.at("materialRetirement"));
      if (value.contains("resourceRetirement"))
        event.resourceRetirement = resourceRetirementRequest(value.at("resourceRetirement"));
      if (value.contains("cameraProjection")) event.cameraProjection=cameraProjectionRequest(value.at("cameraProjection"));
      if (value.contains("cameraPose")) event.cameraPose=cameraPoseRequest(value.at("cameraPose"));
      if (value.contains("cameraSelection")) event.cameraSelection=cameraSelectionRequest(value.at("cameraSelection"));
      if(value.contains("lightEdit")) event.lightEdit=lightEditRequest(value.at("lightEdit"));
      if(value.contains("lightSelection")) event.lightSelection=lightTarget(value.at("lightSelection"));
      if(value.contains("lightAdd")) event.lightAdd=lightAddRequest(value.at("lightAdd"));
      const unsigned lightActions=unsigned(event.lightEdit.has_value())+unsigned(event.lightSelection.has_value())+unsigned(event.lightAdd.has_value());
      if(value.contains("expectLightEditFailure")) {
        if(lightActions!=1 || !value.at("expectLightEditFailure").is_boolean())
          throw std::invalid_argument("Expected light failure requires exactly one light action and a boolean");
        event.expectLightEditFailure=value.at("expectLightEditFailure").get<bool>();
      }
      if(lightActions) {
        if(lightActions!=1) throw std::invalid_argument("Capture light actions require separate events");
        // A typed action has a closed envelope. In particular, false-valued
        // mutation keys cannot bypass isolation by looking inert after parse.
        nodeFields(value,{"frame","lightEdit","lightSelection","lightAdd","expectLightEditFailure",
            "taa","fsr","dlss","exposure","resize","depthProbe","environment","environmentAsync","environmentYaw","proceduralSky","technique","rayShadows","rayShadowSamples","rayShadowDenoise","reloadShaders","gpuTransforms",
            "areaLighting","areaLightSamples","ddgiPaused","ddgiEnabled","ddgiIntensity","samples","reset"},"light action event");
      }
      for(const auto& [field,present,destination]:{
          std::tuple{"expectCameraProjectionFailure",event.cameraProjection.has_value(),&event.expectCameraProjectionFailure},
          std::tuple{"expectCameraSelectionFailure",event.cameraSelection.has_value(),&event.expectCameraSelectionFailure},
          std::tuple{"expectCameraPoseFailure",event.cameraPose.has_value(),&event.expectCameraPoseFailure}}) {
        if(!value.contains(field)) continue;
        if(!present || !value.at(field).is_boolean()) throw std::invalid_argument("Expected camera failure requires its action and a boolean");
        *destination=value.at(field).get<bool>();
      }
      if (value.contains("expectResourceRetirementFailure")) {
        if (!value.at("expectResourceRetirementFailure").is_boolean() || !event.resourceRetirement)
          throw std::invalid_argument("Capture expectResourceRetirementFailure requires resourceRetirement and a boolean");
        event.expectResourceRetirementFailure=value.at("expectResourceRetirementFailure").get<bool>();
      }
      if (value.contains("expectMaterialTextureFailure")) {
        if (!value.at("expectMaterialTextureFailure").is_boolean())
          throw std::invalid_argument("Capture expectMaterialTextureFailure must be a boolean");
        if (!event.materialTextureEdit)
          throw std::invalid_argument("Capture expectMaterialTextureFailure requires materialTextureEdit");
        event.expectMaterialTextureFailure = value.at("expectMaterialTextureFailure").get<bool>();
      }
      if (value.contains("expectMaterialRetirementFailure")) {
        if (!value.at("expectMaterialRetirementFailure").is_boolean())
          throw std::invalid_argument("Capture expectMaterialRetirementFailure must be a boolean");
        if (!event.materialRetirement)
          throw std::invalid_argument("Capture expectMaterialRetirementFailure requires materialRetirement");
        event.expectMaterialRetirementFailure = value.at("expectMaterialRetirementFailure").get<bool>();
      }
      if (value.contains("animationPlaying")) {
        if (!value.at("animationPlaying").is_boolean())
          throw std::invalid_argument("Capture animationPlaying must be a boolean");
        event.animationPlaying = value.at("animationPlaying").get<bool>();
      }
      if (value.contains("animationClip")) {
        const auto& clip = value.at("animationClip");
        if (!clip.is_number_integer() ||
            (clip.is_number_unsigned() ? clip.get<uint64_t>() > uint64_t(std::numeric_limits<int>::max()) :
             (clip.get<int64_t>() < -1 || clip.get<int64_t>() > std::numeric_limits<int>::max())))
          throw std::invalid_argument("Capture animationClip must be an integer in [-1,INT_MAX]");
        event.animationClip = clip.get<int>();
      }
      if (value.contains("animationTime")) {
        const auto& time = value.at("animationTime");
        if (!time.is_number() || !std::isfinite(time.get<double>()) || time.get<double>() < 0)
          throw std::invalid_argument("Capture animationTime must be a finite nonnegative number");
        event.animationTime = time.get<double>();
      }
      if (value.contains("interactivityPlaying")) {
        if (!value.at("interactivityPlaying").is_boolean())
          throw std::invalid_argument("Capture interactivityPlaying must be a boolean");
        event.interactivityPlaying = value.at("interactivityPlaying").get<bool>();
      }
      if (value.contains("interactivityReset")) {
        if (!value.at("interactivityReset").is_boolean())
          throw std::invalid_argument("Capture interactivityReset must be a boolean");
        event.interactivityReset = value.at("interactivityReset").get<bool>();
      }
      if (value.contains("interactivityEvent")) {
        const auto& authored = value.at("interactivityEvent");
        if (!authored.is_string() || authored.get_ref<const std::string&>().empty() ||
            authored.get_ref<const std::string&>().size() > 512)
          throw std::invalid_argument("Capture interactivityEvent must be a nonempty bounded string");
        event.interactivityEvent = authored.get<std::string>();
      }
      if(value.contains("gltfScene")) {
        const auto& scene=value.at("gltfScene");
        if(!scene.is_number_integer() ||
            (scene.is_number_unsigned()?scene.get<uint64_t>()>uint64_t(std::numeric_limits<int>::max()):
             (scene.get<int64_t>()<0 || scene.get<int64_t>()>std::numeric_limits<int>::max())))
          throw std::invalid_argument("Capture gltfScene must be an integer in [0,INT_MAX]");
        event.gltfScene=scene.get<int>();
      }
      if (value.contains("materialVariant")) {
        const auto& variant=value.at("materialVariant");
        if (!variant.is_number_integer() ||
            (variant.is_number_unsigned()?variant.get<uint64_t>()>uint64_t(std::numeric_limits<int>::max()):
             (variant.get<int64_t>() < -1 || variant.get<int64_t>()>std::numeric_limits<int>::max())))
          throw std::invalid_argument("Capture materialVariant must be an integer in [-1,INT_MAX]");
        event.materialVariant=variant.get<int>();
      }
      if (value.contains("sceneHistory")) {
        if (!value.at("sceneHistory").is_string()) throw std::invalid_argument("Capture sceneHistory must be undo or redo");
        event.sceneHistory=value.at("sceneHistory").get<std::string>();
        if (event.sceneHistory!="undo" && event.sceneHistory!="redo") throw std::invalid_argument("Capture sceneHistory must be undo or redo");
      }
      if (event.materialVariant && !event.sceneHistory.empty()) throw std::invalid_argument("Capture variant and history actions must use separate events");
      if (value.contains("nodeFlags")) {
        const auto& flags = value.at("nodeFlags");
        if (!flags.is_object() || !flags.contains("sourceNode"))
          throw std::invalid_argument("Capture nodeFlags requires a sourceNode");
        const auto& source = flags.at("sourceNode");
        if (!source.is_number_integer() ||
            (source.is_number_unsigned() ? source.get<uint64_t>() > uint64_t(std::numeric_limits<int>::max()) :
             (source.get<int64_t>() < 0 || source.get<int64_t>() > std::numeric_limits<int>::max())))
          throw std::invalid_argument("Capture nodeFlags sourceNode must be an integer in [0,INT_MAX]");
        CaptureNodeFlags parsed; parsed.sourceNode = source.get<int>();
        for (auto field = flags.begin(); field != flags.end(); ++field) {
          if (field.key() == "sourceNode") continue;
          if ((field.key() != "visible" && field.key() != "selectable" && field.key() != "hoverable") || !field->is_boolean())
            throw std::invalid_argument("Capture nodeFlags accepts only visible/selectable/hoverable booleans");
        }
        if (flags.contains("visible")) parsed.visible = flags.at("visible").get<bool>();
        if (flags.contains("selectable")) parsed.selectable = flags.at("selectable").get<bool>();
        if (flags.contains("hoverable")) parsed.hoverable = flags.at("hoverable").get<bool>();
        if (!parsed.visible && !parsed.selectable && !parsed.hoverable)
          throw std::invalid_argument("Capture nodeFlags requires at least one flag");
        if (!event.sceneHistory.empty() || event.materialVariant || event.gltfScene)
          throw std::invalid_argument("Capture nodeFlags, scene selection, variant and history actions need separate events");
        event.nodeFlags = parsed;
      }
      auto physicsCommand = [&](const char *name, std::string &target,
                                std::initializer_list<const char *> commands) {
        if (!value.contains(name)) return;
        const auto &command = value.at(name);
        if (!command.is_string())
          throw std::invalid_argument("Capture physics commands must be strings");
        target = command.get<std::string>();
        if (!target.empty() && std::none_of(commands.begin(), commands.end(),
                [&](const char *allowed) { return target == allowed; }))
          throw std::invalid_argument("Unsupported capture physics command");
      };
      physicsCommand("physicsAttach", event.physicsAttach,
                     {"static", "kinematic", "dynamic"});
      physicsCommand("physicsAction", event.physicsAction,
                     {"play", "pause", "step", "reset", "clear", "detach"});
      physicsCommand("physicsJointAction", event.physicsJointAction, {"remove", "clear"});
      if (value.contains("physicsSettings")) {
        if (event.physicsAttach.empty())
          throw std::invalid_argument("Capture body settings require physicsAttach");
        event.physicsSettings = bodySettings(value.at("physicsSettings"));
      }
      if (value.contains("physicsJointCreate"))
        event.physicsJointCreate = jointCreate(value.at("physicsJointCreate"));
      if (value.contains("physicsJointMotorVelocity"))
        event.physicsJointMotorVelocity = finiteNumber(value.at("physicsJointMotorVelocity"));
      if (event.physicsJointCreate &&
          (!event.physicsJointAction.empty() || event.physicsJointMotorVelocity ||
           event.physicsAction == "reset" || event.physicsAction == "clear" ||
           event.physicsAction == "detach"))
        throw std::invalid_argument("Capture joint creation conflicts with retirement or motor replacement");
      if (!event.physicsJointAction.empty() && event.physicsJointMotorVelocity)
        throw std::invalid_argument("Capture cannot retire and update a joint motor together");
      if (!event.physicsAttach.empty() &&
          (event.physicsAction == "reset" || event.physicsAction == "clear" ||
           event.physicsAction == "detach"))
        throw std::invalid_argument(
            "Capture cannot attach and retire/reset physics in one event");
      if (value.contains("physicsNode")) {
        const auto &node = value.at("physicsNode");
        const double index = node.is_number() ? node.get<double>() : -1.0;
        if (!std::isfinite(index) || index < 0.0 ||
            index > double(std::numeric_limits<uint32_t>::max()) ||
            std::floor(index) != index)
          throw std::invalid_argument("Capture physics node must be a uint32 index");
        if (event.physicsAttach.empty() && event.physicsAction != "detach")
          throw std::invalid_argument(
              "Capture physicsNode requires attachment or detach");
        event.physicsNode = node.get<uint32_t>();
      }
      if (value.contains("bimHiddenObject"))
        event.bimHiddenObject = value.at("bimHiddenObject").get<uint32_t>();
      if (value.contains("bimSelectedObject")) {
        const auto &object = value.at("bimSelectedObject");
        const double index = object.is_number() ? object.get<double>() : -1.0;
        if (!std::isfinite(index) || index < 0.0 ||
            index > double(std::numeric_limits<uint32_t>::max()) ||
            std::floor(index) != index)
          throw std::invalid_argument("Capture BIM selection must be a uint32 index");
        event.bimSelectedObject = object.get<uint32_t>();
      }
      if (value.contains("bimLodBias")) {
        event.bimLodBias = value.at("bimLodBias").get<int>();
        if (*event.bimLodBias < -8 || *event.bimLodBias > 8)
          throw std::invalid_argument("BIM LOD bias must be in [-8,8]");
      }
      if (value.contains("sectionPlane")) {
        const auto &plane = value.at("sectionPlane");
        if (!plane.is_array() || plane.size() != 4)
          throw std::invalid_argument("Section plane needs four components");
        glm::vec4 equation(plane[0].get<float>(), plane[1].get<float>(),
                           plane[2].get<float>(), plane[3].get<float>());
        if (!std::isfinite(equation.x) || !std::isfinite(equation.y) ||
            !std::isfinite(equation.z) || !std::isfinite(equation.w) ||
            glm::length(glm::vec3(equation)) < 1e-4f)
          throw std::invalid_argument(
              "Section plane must have a finite nonzero normal");
        event.sectionPlane = equation / glm::length(glm::vec3(equation));
      }
      if (value.contains("areaLightPosition"))
        event.areaLightPosition = vec3(value.at("areaLightPosition"));
      event.rayShadows = value.value("rayShadows", std::string{});
      if (!event.rayShadows.empty() && event.rayShadows != "raster" &&
          event.rayShadows != "hard" && event.rayShadows != "soft")
        throw std::invalid_argument("Invalid capture ray shadow mode");
      if (value.contains("rayShadowSamples")) {
        event.rayShadowSamples = value.at("rayShadowSamples").get<uint32_t>();
        if (!*event.rayShadowSamples || *event.rayShadowSamples > 32)
          throw std::invalid_argument("Invalid capture ray shadow samples");
      }
      if (value.contains("rayShadowDenoise"))
        event.rayShadowDenoise = value.at("rayShadowDenoise").get<bool>();
      if (value.contains("areaLighting")) {
        const auto &mode = value.at("areaLighting");
        if (!mode.is_string() || (mode != "ltc" && mode != "sampled"))
          throw std::invalid_argument("Invalid capture area lighting mode");
        event.areaLighting = mode.get<std::string>();
      }
      if (value.contains("areaLightSamples")) {
        const auto &samples = value.at("areaLightSamples");
        // Check the original numeric value before conversion can truncate a
        // fraction or wrap an out-of-range integer into an allowed count.
        if (!samples.is_number() ||
            (samples != 9u && samples != 25u && samples != 64u))
          throw std::invalid_argument("Capture area light samples must be 9,25,64");
        event.areaLightSamples = samples.get<uint32_t>();
      }
      auto boolean = [&](const char *name, std::optional<bool> &target) {
        if (value.contains(name))
          target = value.at(name).get<bool>();
      };
      boolean("taa", event.taa);
      const bool hasFsr = value.contains("fsr");
      const bool hasDlss = value.contains("dlss");
      if (hasFsr && hasDlss)
        throw std::invalid_argument("A capture event selects only one temporal upscaler");
      if (hasFsr || hasDlss) {
        const char* key = hasFsr ? "fsr" : "dlss";
        const auto& requested = value.at(key);
        const auto mode = requested.is_string() ?
            container::renderer::parseUpscaleMode(requested.get<std::string>()) : std::nullopt;
        if (!mode)
          throw std::invalid_argument(std::string("Capture ") + key + " expects native, quality, balanced, or performance");
        event.upscaling = container::renderer::UpscaleSelection{
            hasFsr ? container::renderer::UpscaleBackend::Fsr : container::renderer::UpscaleBackend::Dlss, *mode};
      }
      boolean("orthographic", event.orthographic);
      boolean("objectVisible", event.objectVisible);
      boolean("minimized", event.minimized);
      for (const char *name : {"ddgiPaused", "ddgiEnabled"}) {
        if (value.contains(name) && !value.at(name).is_boolean())
          throw std::invalid_argument("Capture DDGI flags must be booleans");
      }
      boolean("ddgiPaused", event.ddgiPaused);
      boolean("ddgiEnabled", event.ddgiEnabled);
      if (value.contains("ddgiIntensity")) {
        const auto &input = value.at("ddgiIntensity");
        const double intensity = input.is_number() ? input.get<double>() : -1.0;
        if (!std::isfinite(intensity) || intensity < 0.0 ||
            intensity > container::renderer::kMaxDdgiIntensity)
          throw std::invalid_argument(
              "Capture ddgiIntensity must be a finite number in [0,10000]");
        event.ddgiIntensity = static_cast<float>(intensity);
      }
      if (value.contains("ddgiFitProbes")) {
        const auto &probes = value.at("ddgiFitProbes");
        const double count = probes.is_number() ? probes.get<double>() : -1.0;
        // JSON Schema's integer type includes exact integral numbers such as
        // 27.0. Validate that value before narrowing, matching other captures.
        if (!std::isfinite(count) || count < 27.0 ||
            count > double(std::numeric_limits<uint32_t>::max()) ||
            std::floor(count) != count)
          throw std::invalid_argument(
              "Capture ddgiFitProbes must be a uint32 integer >=27");
        event.ddgiFitProbes = probes.get<uint32_t>();
      }
      if (value.contains("samples")) {
        event.samples = value.at("samples").get<uint32_t>();
        if (*event.samples != 1 && *event.samples != 2 && *event.samples != 4 &&
            *event.samples != 8)
          throw std::invalid_argument("Capture MSAA samples must be 1,2,4,8");
      }
      if (value.contains("exposure")) {
        event.exposure = value.at("exposure").get<float>();
        if (!std::isfinite(*event.exposure) || *event.exposure < 0)
          throw std::invalid_argument("Invalid capture exposure");
      }
      if (value.contains("resize")) {
        const auto &size = value.at("resize");
        if (!size.is_array() || size.size() != 2)
          throw std::invalid_argument("Capture resize requires two dimensions");
        event.resize =
            glm::uvec2(size[0].get<uint32_t>(), size[1].get<uint32_t>());
        if (!event.resize->x || !event.resize->y || event.resize->x > 16384 ||
            event.resize->y > 16384)
          throw std::invalid_argument("Invalid capture resize dimensions");
      }
      if (value.contains("reloadShaders"))
        event.reloadShaders = value.at("reloadShaders").get<bool>();
      if (value.contains("referenceAutofocus"))
        event.referenceAutofocus = value.at("referenceAutofocus").get<bool>();
      if (value.contains("referenceDisplay")) {
        const auto& mode = value.at("referenceDisplay");
        if (mode.is_string())
          event.referenceDisplay = container::renderer::parseReferencePathDisplay(mode.get<std::string>());
        if (!event.referenceDisplay) throw std::invalid_argument("Invalid capture referenceDisplay mode");
      }
      if (value.contains("referenceOidn")) {
        event.referenceOidn = container::renderer::parseReferenceOidnQuality(value.at("referenceOidn").get<std::string>());
        if (!event.referenceOidn) throw std::invalid_argument("Invalid capture referenceOidn quality");
      }
      if (value.contains("referenceOptix")) {
        if (event.referenceOidn || !value.at("referenceOptix").is_boolean())
          throw std::invalid_argument("Capture referenceOptix requires a boolean and a separate backend event");
        event.referenceOptix = value.at("referenceOptix").get<bool>();
      }
      if (value.contains("referenceDlss")) {
        if (event.referenceOidn || event.referenceOptix)
          throw std::invalid_argument("Capture referenceDlss requires a separate backend event");
        event.referenceDlss = container::renderer::parseReferenceDlssQuality(value.at("referenceDlss").get<std::string>());
        if (!event.referenceDlss) throw std::invalid_argument("Invalid capture referenceDlss quality");
      }
      if (value.contains("comfyUi")) {
        const auto& job = value.at("comfyUi");
        nodeFields(job, {"action", "prompt", "seed", "wait"}, "ComfyUI capture action");
        const auto action = job.at("action").get<std::string>();
        CaptureComfyUi request;
        if (action == "generate-environment") request.action = CaptureComfyUi::Action::GenerateEnvironment;
        else if (action == "restyle-viewport") request.action = CaptureComfyUi::Action::RestyleViewport;
        else if (action == "apply-environment") request.action = CaptureComfyUi::Action::ApplyEnvironment;
        else if (action == "cancel") request.action = CaptureComfyUi::Action::Cancel;
        else throw std::invalid_argument("Unknown ComfyUI capture action");
        if (request.action == CaptureComfyUi::Action::GenerateEnvironment ||
            request.action == CaptureComfyUi::Action::RestyleViewport) {
          request.prompt = job.at("prompt").get<std::string>();
          if (request.prompt.size() > 2048 || request.prompt.find_first_not_of(" \t\r\n") == std::string::npos)
            throw std::invalid_argument("ComfyUI capture requires a bounded nonempty prompt");
          if (job.contains("seed")) {
            const auto& seed = job.at("seed");
            if (!seed.is_number_integer() || (!seed.is_number_unsigned() && seed.get<int64_t>() < 0))
              throw std::invalid_argument("ComfyUI capture seed must be a nonnegative integer");
            request.seed = seed.get<uint64_t>();
          }
        } else if (job.contains("prompt") || job.contains("seed")) {
          throw std::invalid_argument("Only a ComfyUI generation action accepts a prompt or seed");
        }
        request.wait = job.value("wait", false);
        event.comfyUi = std::move(request);
      }
      if (value.contains("gpuTransforms"))
        event.gpuTransforms = value.at("gpuTransforms").get<bool>();
      if (value.contains("depthProbe")) {
        const auto &pixel = value.at("depthProbe");
        if (!pixel.is_array() || pixel.size() != 2)
          throw std::invalid_argument("Capture depthProbe requires two pixel coordinates");
        const auto coordinate = [](const nlohmann::json &component) {
          if (!component.is_number_integer() || component.get<double>() < 0 ||
              component.get<double>() > 16384)
            throw std::invalid_argument("Capture depthProbe coordinates must be integers in [0,16384]");
          return component.get<uint32_t>();
        };
        event.depthProbe = glm::uvec2(coordinate(pixel[0]), coordinate(pixel[1]));
      }
      event.technique = value.value("technique", std::string{});
      if (!event.technique.empty() && event.technique != "forward-raster" &&
          event.technique != "deferred-raster" && event.technique != "path-tracing")
        throw std::invalid_argument("Invalid capture render technique");
      event.reload = value.value("reload", std::string{});
      if (value.contains("environmentYaw")) {
        const auto& yaw = value.at("environmentYaw");
        if (!yaw.is_number() || !std::isfinite(yaw.get<double>()) ||
            std::abs(yaw.get<double>()) > 180.0)
          throw std::invalid_argument("Capture environmentYaw requires degrees in [-180,180]");
        event.environmentYaw = yaw.get<float>();
      }
      if (value.contains("environment")) {
        event.environment = value.at("environment").get<std::string>();
        if (event.environment.empty() || event.environment.size() > 4096 ||
            event.environment.find('\0') != std::string::npos)
          throw std::invalid_argument("Capture environment requires a nonempty HDR path of at most 4096 bytes");
      }
      if (value.contains("environmentAsync")) {
        if (event.environment.empty() || !value.at("environmentAsync").is_boolean())
          throw std::invalid_argument("Capture environmentAsync requires an environment path and a boolean");
        event.environmentAsync = value.at("environmentAsync").get<bool>();
      }
      if (value.contains("proceduralSky")) {
        const auto mode = value.at("proceduralSky").get<std::string>();
        using container::renderer::SkyMode;
        if (!event.environment.empty() || (mode != "plain" && mode != "gradient" && mode != "atmosphere"))
          throw std::invalid_argument("Capture proceduralSky requires plain, gradient, or atmosphere without an HDR path");
        event.proceduralSky = mode == "plain" ? SkyMode::Plain :
            mode == "gradient" ? SkyMode::Gradient : SkyMode::Atmosphere;
      }
      event.guiReload = value.value("guiReload", std::string{});
      event.guiSampleModel = value.value("guiSampleModel", std::string{});
      if (value.contains("guiUnload") && !value.at("guiUnload").is_boolean())
        throw std::invalid_argument("Capture guiUnload must be a boolean");
      event.guiUnload = value.value("guiUnload", false);
      if (unsigned(!event.reload.empty()) + unsigned(!event.guiReload.empty()) +
              unsigned(!event.guiSampleModel.empty()) + unsigned(event.guiUnload) + unsigned(event.merge.has_value()) >
          1u)
        throw std::invalid_argument("Choose one model reload or merge route per event");
      if (event.merge && (event.gltfScene || event.materialVariant || event.nodeFlags ||
          !event.sceneHistory.empty() || event.objectVisible || event.bimHiddenObject ||
          !event.physicsAttach.empty() || !event.physicsAction.empty() || event.physicsJointCreate ||
          !event.physicsJointAction.empty() || event.physicsJointMotorVelocity))
        throw std::invalid_argument("Capture merge and other scene/history/physics mutations need separate events");
      if (event.nodeAuthoring && (event.merge || event.gltfScene || event.materialVariant || event.nodeFlags ||
          !event.sceneHistory.empty() || !event.reload.empty() || !event.guiReload.empty() ||
          !event.guiSampleModel.empty() || event.guiUnload || event.objectVisible || event.bimHiddenObject ||
          !event.physicsAttach.empty() || !event.physicsAction.empty() || event.physicsJointCreate ||
          !event.physicsJointAction.empty() || event.physicsJointMotorVelocity))
        throw std::invalid_argument("Capture node authoring and other model/scene/history/physics mutations need separate events");
      const bool animationControl = event.animationPlaying || event.animationClip || event.animationTime ||
          event.interactivityPlaying || event.interactivityReset || event.interactivityEvent;
      if((event.cameraProjection || event.cameraSelection || event.cameraPose) &&
          (unsigned(event.cameraProjection.has_value())+unsigned(event.cameraSelection.has_value())+unsigned(event.cameraPose.has_value())>1 ||
          event.resourceRetirement || event.materialRetirement || event.materialEdit || event.materialBinding || event.materialTextureEdit ||
          event.merge || event.gltfScene || event.materialVariant || event.nodeFlags || event.nodeAuthoring || event.nodeReparent ||
          event.nodeTransform || event.addPrimitive || animationControl || !event.sceneHistory.empty() || !event.reload.empty() ||
          !event.guiReload.empty() || !event.guiSampleModel.empty() || event.guiUnload || event.objectVisible || event.bimHiddenObject ||
          !event.physicsAttach.empty() || !event.physicsAction.empty() || event.physicsJointCreate ||
          !event.physicsJointAction.empty() || event.physicsJointMotorVelocity || event.physicsSettings || event.physicsNode))
        throw std::invalid_argument("Capture authored camera actions and other scene/history/timeline mutations need separate events");
      if (event.resourceRetirement && (event.materialRetirement || event.materialEdit || event.materialBinding || event.materialTextureEdit ||
          event.merge || event.gltfScene || event.materialVariant || event.nodeFlags || event.nodeAuthoring || event.nodeReparent ||
          event.nodeTransform || event.addPrimitive || animationControl || !event.sceneHistory.empty() || !event.reload.empty() ||
          !event.guiReload.empty() || !event.guiSampleModel.empty() || event.guiUnload || event.objectVisible || event.bimHiddenObject ||
          !event.physicsAttach.empty() || !event.physicsAction.empty() || event.physicsJointCreate || !event.physicsJointAction.empty() ||
          event.physicsJointMotorVelocity || event.physicsSettings || event.physicsNode))
        throw std::invalid_argument("Capture resource retirement and model/scene/material/history/timeline/physics mutations need separate events");
      if (event.materialEdit && (event.materialRetirement || event.materialTextureEdit || event.materialBinding || event.merge || event.gltfScene || event.materialVariant || event.nodeFlags ||
          event.nodeAuthoring || event.nodeReparent || event.nodeTransform || event.addPrimitive || animationControl ||
          !event.sceneHistory.empty() || !event.reload.empty() || !event.guiReload.empty() ||
          !event.guiSampleModel.empty() || event.guiUnload || event.objectVisible || event.bimHiddenObject ||
          !event.physicsAttach.empty() || !event.physicsAction.empty() || event.physicsJointCreate ||
          !event.physicsJointAction.empty() || event.physicsJointMotorVelocity || event.physicsSettings || event.physicsNode))
        throw std::invalid_argument("Capture material edits and model/scene/history/timeline/physics mutations need separate events");
      if (event.materialBinding && (event.materialRetirement || event.materialTextureEdit || event.materialEdit || event.merge || event.gltfScene || event.materialVariant || event.nodeFlags ||
          event.nodeAuthoring || event.nodeReparent || event.nodeTransform || event.addPrimitive || animationControl ||
          !event.sceneHistory.empty() || !event.reload.empty() || !event.guiReload.empty() ||
          !event.guiSampleModel.empty() || event.guiUnload || event.objectVisible || event.bimHiddenObject ||
          !event.physicsAttach.empty() || !event.physicsAction.empty() || event.physicsJointCreate ||
          !event.physicsJointAction.empty() || event.physicsJointMotorVelocity || event.physicsSettings || event.physicsNode))
        throw std::invalid_argument("Capture material binding and model/scene/history/timeline/physics mutations need separate events");
      if (event.materialTextureEdit && (event.materialRetirement || event.materialEdit || event.materialBinding || event.merge || event.gltfScene || event.materialVariant || event.nodeFlags ||
          event.nodeAuthoring || event.nodeReparent || event.nodeTransform || event.addPrimitive || animationControl ||
          !event.sceneHistory.empty() || !event.reload.empty() || !event.guiReload.empty() ||
          !event.guiSampleModel.empty() || event.guiUnload || event.objectVisible || event.bimHiddenObject ||
          !event.physicsAttach.empty() || !event.physicsAction.empty() || event.physicsJointCreate ||
          !event.physicsJointAction.empty() || event.physicsJointMotorVelocity || event.physicsSettings || event.physicsNode))
        throw std::invalid_argument("Capture texture edits and model/scene/material/history/timeline/physics mutations need separate events");
      if (event.materialRetirement && (event.materialEdit || event.materialBinding || event.materialTextureEdit || event.merge || event.gltfScene || event.materialVariant || event.nodeFlags ||
          event.nodeAuthoring || event.nodeReparent || event.nodeTransform || event.addPrimitive || animationControl ||
          !event.sceneHistory.empty() || !event.reload.empty() || !event.guiReload.empty() ||
          !event.guiSampleModel.empty() || event.guiUnload || event.objectVisible || event.bimHiddenObject ||
          !event.physicsAttach.empty() || !event.physicsAction.empty() || event.physicsJointCreate ||
          !event.physicsJointAction.empty() || event.physicsJointMotorVelocity || event.physicsSettings || event.physicsNode))
        throw std::invalid_argument("Capture material retirement and model/scene/material/history/timeline/physics mutations need separate events");
      const unsigned authoringCommands = unsigned(event.nodeAuthoring.has_value()) + unsigned(event.nodeReparent.has_value()) +
          unsigned(event.nodeTransform.has_value()) + unsigned(event.addPrimitive.has_value()) + unsigned(animationControl);
      if (authoringCommands > 1)
        throw std::invalid_argument("Capture node edits and animation controls need separate events");
      if ((event.nodeReparent || event.nodeTransform || event.addPrimitive || animationControl) && (event.merge || event.gltfScene || event.materialVariant || event.nodeFlags ||
          !event.sceneHistory.empty() || !event.reload.empty() || !event.guiReload.empty() ||
          !event.guiSampleModel.empty() || event.guiUnload || event.objectVisible || event.bimHiddenObject ||
          !event.physicsAttach.empty() || !event.physicsAction.empty() || event.physicsJointCreate ||
          !event.physicsJointAction.empty() || event.physicsJointMotorVelocity || event.physicsSettings || event.physicsNode))
        throw std::invalid_argument("Capture node edits/animation controls and model/scene/history/physics mutations need separate events");
      if (value.contains("guiWaitForLoad") &&
          !value.at("guiWaitForLoad").is_boolean())
        throw std::invalid_argument("Capture guiWaitForLoad must be a boolean");
      event.guiWaitForLoad = value.value("guiWaitForLoad", true);
      if (!event.guiWaitForLoad && event.guiReload.empty() &&
          event.guiSampleModel.empty() && !event.guiUnload && !event.merge)
        throw std::invalid_argument(
            "Capture guiWaitForLoad=false requires a GUI model action");
      events_.push_back(std::move(event));
    }
    validateOrder(events_);
  }

  CaptureSample sample(uint32_t frame) const {
    checkFrame(frame);
    CaptureSample result;
    result.objectNode = objectNode_;
    result.objectName = objectName_;
    if (!cameras_.empty()) {
      result.camera = interpolate(
          cameras_, frame,
          [](const CaptureCamera &a, const CaptureCamera &b, float t) {
            return CaptureCamera{mixVector(a.position, b.position, t),
                                 mixVector(a.target, b.target, t)};
          });
    }
    if (!objects_.empty())
      result.objectTranslation =
          interpolate(objects_, frame, [](auto a, auto b, float t) {
            return mixVector(a, b, t);
          });
    if (!scales_.empty())
      result.objectScale =
          interpolate(scales_, frame, [](auto a, auto b, float t) {
            return mixVector(a, b, t);
          });
    if (!bim_.empty())
      result.bimTranslation =
          interpolate(bim_, frame, [](auto a, auto b, float t) {
            return mixVector(a, b, t);
          });
    for (const auto &event : events_)
      if (event.frame == frame)
        result.event = event;
    return result;
  }
  bool captures(uint32_t frame) const {
    return std::find(sampleFrames.begin(), sampleFrames.end(), frame) !=
           sampleFrames.end();
  }
  [[nodiscard]] bool hasTelemetryFrames() const {
    return !telemetryFrames_.empty();
  }
  [[nodiscard]] bool capturesTelemetry(uint32_t frame) const {
    return std::ranges::binary_search(telemetryFrames_, frame);
  }
  uint32_t frameCount{0};
  std::vector<uint32_t> sampleFrames;

private:
  std::vector<uint32_t> telemetryFrames_;

  static float lightNumber(const nlohmann::json& value) {
    if(!value.is_number()) throw std::invalid_argument("Capture light values require numeric float components");
    const double number=value.get<double>();
    if(!std::isfinite(number) || std::abs(number)>std::numeric_limits<float>::max())
      throw std::invalid_argument("Capture light value exceeds finite float range");
    return float(number);
  }
  template<glm::length_t N> static glm::vec<N,float,glm::defaultp> lightVector(const nlohmann::json& value) {
    if(!value.is_array() || value.size()!=size_t(N)) throw std::invalid_argument("Capture light vector has the wrong shape");
    glm::vec<N,float,glm::defaultp> result{};for(glm::length_t i=0;i<N;++i) result[i]=lightNumber(value[size_t(i)]);return result;
  }
  static container::renderer::EditableLightType lightType(const nlohmann::json& value) {
    using T=container::renderer::EditableLightType;
    if(!value.is_string()) throw std::invalid_argument("Capture light type must be a canonical string");
    const auto& type=value.get_ref<const std::string&>();
    if(type=="point") return T::Point;if(type=="spot") return T::Spot;if(type=="area") return T::Area;
    if(type=="directional") return T::Directional;throw std::invalid_argument("Unknown capture light type");
  }
  static CaptureLightTarget lightTarget(const nlohmann::json& value) {
    using S=container::renderer::EditableLightSource;
    nodeFields(value,{"type","source","sourceNode","nodeName","index"},"light target");
    if(!value.contains("type") || !value.contains("source") || !value.at("source").is_string())
      throw std::invalid_argument("Capture light target requires canonical type and source");
    CaptureLightTarget result;result.type=lightType(value.at("type"));const auto& source=value.at("source").get_ref<const std::string&>();
    if(source=="imported") result.source=S::Imported;else if(source=="manual") result.source=S::Manual;
    else if(source=="generated") result.source=S::Generated;else throw std::invalid_argument("Unknown capture light source");
    if(unsigned(value.contains("sourceNode"))+unsigned(value.contains("nodeName"))+unsigned(value.contains("index"))!=1)
      throw std::invalid_argument("Capture light target requires exactly one selector");
    if(result.source==S::Imported) {
      if(value.contains("sourceNode")) result.sourceNode=int32_t(cameraInteger(value.at("sourceNode"),0,INT32_MAX));
      else if(value.contains("nodeName")) {
        const auto& name=value.at("nodeName");if(!name.is_string() || name.get_ref<const std::string&>().size()>4096)
          throw std::invalid_argument("Capture light nodeName must be a bounded string");
        container::scene::ValidateSceneNodeName(name.get_ref<const std::string&>());result.nodeName=name.get<std::string>();
      } else throw std::invalid_argument("Imported capture lights require sourceNode or nodeName");
    } else {
      if(!value.contains("index")) throw std::invalid_argument("Manual and generated capture lights require index");
      result.index=uint32_t(cameraInteger(value.at("index"),0,UINT32_MAX-1));
    }
    return result;
  }
  static container::renderer::LightEditPatch lightValues(const nlohmann::json& value,container::renderer::EditableLightType type) {
    using namespace container::renderer;
    nodeFields(value,{"position","direction","color","intensity","range","spotCone","areaSize","areaShape","areaBasis"},"light values");
    if(value.empty()) throw std::invalid_argument("Capture light values must contain an edit");
    LightEditPatch result;auto& v=result.values;
    const auto add=[&](LightEditField field){result.fields|=LightEditFieldBit(field);};
    if(value.contains("position")) {v.position=lightVector<3>(value.at("position"));add(LightEditField::Position);}
    if(value.contains("direction")) {
      v.direction=lightVector<3>(value.at("direction"));const double length=glm::dot(glm::dvec3(v.direction),glm::dvec3(v.direction));
      if(length<=1e-12) throw std::invalid_argument("Capture light direction must be nonzero");add(LightEditField::Direction);
    }
    if(value.contains("color")) {v.color=lightVector<3>(value.at("color"));if(glm::any(glm::lessThan(v.color,glm::vec3(0))))
      throw std::invalid_argument("Capture light color must be nonnegative");add(LightEditField::Color);}
    if(value.contains("intensity")) {v.intensity=lightNumber(value.at("intensity"));if(v.intensity<0) throw std::invalid_argument("Capture light intensity must be nonnegative");add(LightEditField::Intensity);}
    if(value.contains("range")) {v.range=lightNumber(value.at("range"));if(v.range<0) throw std::invalid_argument("Capture light range must be nonnegative");add(LightEditField::Range);}
    if(value.contains("spotCone")) {const auto cone=lightVector<2>(value.at("spotCone"));if(cone.x<0 || cone.y<=0 || cone.x>cone.y || cone.y>179)
      throw std::invalid_argument("Capture light cone requires 0 <= inner <= outer <= 179 degrees and positive outer");
      v.innerConeDegrees=cone.x;v.outerConeDegrees=cone.y;add(LightEditField::SpotCone);}
    if(value.contains("areaSize")) {const auto size=lightVector<2>(value.at("areaSize"));v.areaHalfSize=size*.5f;
      if(glm::any(glm::lessThanEqual(v.areaHalfSize,glm::vec2(0)))) throw std::invalid_argument("Capture area size must be positive full dimensions");add(LightEditField::AreaSize);}
    if(value.contains("areaShape")) {const auto& shape=value.at("areaShape");if(!shape.is_string()) throw std::invalid_argument("Capture area shape must be rectangle or disk");
      const auto& name=shape.get_ref<const std::string&>();if(name=="rectangle") v.areaShape=container::gpu::kAreaLightTypeRectangle;
      else if(name=="disk") v.areaShape=container::gpu::kAreaLightTypeDisk;else throw std::invalid_argument("Unknown capture area shape");add(LightEditField::AreaShape);}
    if(value.contains("areaBasis")) {const auto& basis=value.at("areaBasis");nodeFields(basis,{"tangent","bitangent"},"area basis");
      if(basis.size()!=2) throw std::invalid_argument("Capture area basis requires tangent and bitangent");
      v.tangent=lightVector<3>(basis.at("tangent"));v.bitangent=lightVector<3>(basis.at("bitangent"));
      const auto t=glm::dvec3(v.tangent),b=glm::dvec3(v.bitangent);
      if(glm::dot(t,t)<=1e-12 || glm::dot(b,b)<=1e-12 || glm::dot(glm::cross(t,b),glm::cross(t,b))<=1e-12)
        throw std::invalid_argument("Capture area basis vectors must be nondegenerate");
      // A standalone right-handed basis defines its normal. Explicit
      // direction is retained when present; the backend orthonormalizes it.
      if(!value.contains("direction")) v.direction=glm::vec3(glm::normalize(glm::cross(t,b)));
      add(LightEditField::AreaBasis);add(LightEditField::Direction);}
    uint32_t allowed=LightEditFieldBit(LightEditField::Color)|LightEditFieldBit(LightEditField::Intensity);
    if(type!=EditableLightType::Directional) allowed|=LightEditFieldBit(LightEditField::Position)|LightEditFieldBit(LightEditField::Range);
    if(type!=EditableLightType::Point) allowed|=LightEditFieldBit(LightEditField::Direction);
    if(type==EditableLightType::Spot) allowed|=LightEditFieldBit(LightEditField::SpotCone);
    if(type==EditableLightType::Area) allowed|=LightEditFieldBit(LightEditField::AreaSize)|LightEditFieldBit(LightEditField::AreaShape)|LightEditFieldBit(LightEditField::AreaBasis);
    if(result.fields&~allowed) throw std::invalid_argument("Capture property is unsupported by this light type");return result;
  }
  static CaptureLightGizmo lightGizmo(const nlohmann::json& value) {
    nodeFields(value,{"tool","amount","space","axis","snap"},"light gizmo");
    if(!value.contains("tool") || !value.at("tool").is_string() || !value.contains("amount")) throw std::invalid_argument("Capture light gizmo requires tool and amount");
    CaptureLightGizmo result;const auto& tool=value.at("tool").get_ref<const std::string&>();
    if(tool=="translate") result.tool=CaptureLightGizmo::Tool::Translate;else if(tool=="rotate") result.tool=CaptureLightGizmo::Tool::Rotate;
    else if(tool=="scale") result.tool=CaptureLightGizmo::Tool::Scale;else throw std::invalid_argument("Unknown capture light gizmo tool");
    result.amount=lightVector<3>(value.at("amount"));
    if(value.contains("space")) {if(!value.at("space").is_string()) throw std::invalid_argument("Capture light gizmo space must be world or local");
      const auto& space=value.at("space").get_ref<const std::string&>();if(space=="local") result.space=CaptureLightGizmo::Space::Local;
      else if(space!="world") throw std::invalid_argument("Unknown capture light gizmo space");}
    if(value.contains("axis")) {if(!value.at("axis").is_string()) throw std::invalid_argument("Capture light gizmo axis must be free, x, y or z");
      const auto& axis=value.at("axis").get_ref<const std::string&>();if(axis=="x") result.axis=CaptureLightGizmo::Axis::X;
      else if(axis=="y") result.axis=CaptureLightGizmo::Axis::Y;else if(axis=="z") result.axis=CaptureLightGizmo::Axis::Z;
      else if(axis!="free") throw std::invalid_argument("Unknown capture light gizmo axis");}
    if(value.contains("snap")) {if(!value.at("snap").is_boolean()) throw std::invalid_argument("Capture light gizmo snap must be boolean");result.snap=value.at("snap").get<bool>();}
    return result;
  }
  static CaptureLightEdit lightEditRequest(const nlohmann::json& value) {
    using Phase=container::renderer::LightEditPhase;
    nodeFields(value,{"target","phase","gesture","expectedLightRevision","values","gizmo"},"light edit");
    if(!value.contains("target") || !value.contains("phase") || !value.at("phase").is_string()) throw std::invalid_argument("Capture light edit requires target and phase");
    CaptureLightEdit result;result.target=lightTarget(value.at("target"));const auto& phase=value.at("phase").get_ref<const std::string&>();
    if(phase=="commit") result.phase=Phase::Commit;else if(phase=="begin") result.phase=Phase::Begin;else if(phase=="update") result.phase=Phase::Update;
    else if(phase=="end") result.phase=Phase::End;else if(phase=="cancel") result.phase=Phase::Cancel;else throw std::invalid_argument("Unknown capture light edit phase");
    if(result.phase==Phase::Commit) {if(value.contains("gesture")) throw std::invalid_argument("Capture light commit does not carry a gesture");}
    else {if(!value.contains("gesture")) throw std::invalid_argument("Capture light gesture phase requires gesture identity");result.gesture=cameraInteger(value.at("gesture"),1,9007199254740991ull);}
    if(value.contains("expectedLightRevision")) result.expectedLightRevision=cameraInteger(value.at("expectedLightRevision"),1,9007199254740991ull);
    const auto payloads=unsigned(value.contains("values"))+unsigned(value.contains("gizmo"));
    if((result.phase==Phase::End || result.phase==Phase::Cancel)?payloads!=0:payloads!=1)
      throw std::invalid_argument("Capture light commit/begin/update require exactly one payload; end/cancel carry none");
    if(value.contains("values")) result.values=lightValues(value.at("values"),result.target.type);
    if(value.contains("gizmo")) result.gizmo=lightGizmo(value.at("gizmo"));return result;
  }
  static CaptureLightAdd lightAddRequest(const nlohmann::json& value) {
    nodeFields(value,{"type","expectedLightRevision"},"light add");if(!value.contains("type")) throw std::invalid_argument("Capture light add requires type");
    CaptureLightAdd result;result.type=lightType(value.at("type"));
    if(value.contains("expectedLightRevision")) result.expectedLightRevision=cameraInteger(value.at("expectedLightRevision"),1,9007199254740991ull);return result;
  }
  static uint64_t cameraInteger(const nlohmann::json& value,uint64_t minimum,uint64_t maximum) {
    if(!value.is_number_integer() || (value.is_number_unsigned()?value.get<uint64_t>()>maximum:
        value.get<int64_t>()<0 || uint64_t(value.get<int64_t>())>maximum) || value.get<uint64_t>()<minimum)
      throw std::invalid_argument("Capture camera identity must be an integer within its declared bounds");
    return value.get<uint64_t>();
  }
  static CaptureCameraSelection cameraSelectionRequest(const nlohmann::json& value) {
    nodeFields(value,{"sourceNode","nodeName","viewport"},"cameraSelection");
    if(value.size()!=1) throw std::invalid_argument("Capture cameraSelection requires exactly one selector");
    CaptureCameraSelection result;
    if(value.contains("sourceNode")) result.sourceNode=int32_t(cameraInteger(value.at("sourceNode"),0,INT32_MAX));
    else if(value.contains("nodeName")) {
      const auto& name=value.at("nodeName");
      if(!name.is_string() || name.get_ref<const std::string&>().size()>4096)
        throw std::invalid_argument("Capture camera nodeName must be a bounded string");
      container::scene::ValidateSceneNodeName(name.get_ref<const std::string&>());result.nodeName=name.get<std::string>();
    } else {
      if(!value.at("viewport").is_boolean() || !value.at("viewport").get<bool>())
        throw std::invalid_argument("Capture viewport camera selection must be true");
      result.viewport=true;
    }
    return result;
  }
  static CaptureCameraPose cameraPoseRequest(const nlohmann::json& value) {
    if(!value.is_object()) throw std::invalid_argument("Capture cameraPose must be an object");
    nodeFields(value,{"sourceNode","fromViewport","expectedCameraRevision"},"cameraPose");
    if(!value.contains("sourceNode") || !value.contains("fromViewport") ||
        !value.at("fromViewport").is_boolean() || !value.at("fromViewport").get<bool>())
      throw std::invalid_argument("Capture cameraPose requires sourceNode and fromViewport:true");
    CaptureCameraPose result;result.sourceNode=int32_t(cameraInteger(value.at("sourceNode"),0,INT32_MAX));
    if(value.contains("expectedCameraRevision"))
      result.expectedCameraRevision=cameraInteger(value.at("expectedCameraRevision"),1,9007199254740991ull);
    return result;
  }
  static CaptureCameraProjection cameraProjectionRequest(const nlohmann::json& value) {
    using namespace container::geometry::gltf;
    nodeFields(value,{"sourceNode","expectedCameraRevision","type","values"},"cameraProjection");
    if(!value.contains("sourceNode")) throw std::invalid_argument("Capture cameraProjection requires sourceNode");
    CaptureCameraProjection result;result.sourceNode=int32_t(cameraInteger(value.at("sourceNode"),0,INT32_MAX));
    if(value.contains("expectedCameraRevision")) result.expectedCameraRevision=cameraInteger(value.at("expectedCameraRevision"),1,9007199254740991ull);
    if(value.contains("type")) {
      if(!value.at("type").is_string()) throw std::invalid_argument("Capture camera type must be perspective or orthographic");
      const auto& type=value.at("type").get_ref<const std::string&>();
      if(type=="perspective") result.patch.type=GltfCameraProjection::Perspective;
      else if(type=="orthographic") result.patch.type=GltfCameraProjection::Orthographic;
      else throw std::invalid_argument("Capture camera type is unsupported");
    }
    if(value.contains("values")) {
      const auto& values=value.at("values");
      if(!values.is_array() || values.size()>8) throw std::invalid_argument("Capture camera values require at most eight edits");
      std::array<bool,8> seen{};
      for(const auto& edit:values) {
        nodeFields(edit,{"property","value"},"camera property");
        if(edit.size()!=2 || !edit.contains("property") || !edit.at("property").is_string() || !edit.contains("value"))
          throw std::invalid_argument("Capture camera property requires property and value");
        const auto& name=edit.at("property").get_ref<const std::string&>();
        if(name.size()>64) throw std::invalid_argument("Capture camera property name is too long");
        const auto property=ParseGltfCameraProperty(name);
        if(!property || seen[size_t(*property)]) throw std::invalid_argument("Capture camera property is unknown or duplicated");
        seen[size_t(*property)]=true;
        if(result.patch.type && (size_t(*property)<4)!=(*result.patch.type==GltfCameraProjection::Perspective))
          throw std::invalid_argument("Capture camera value belongs to another projection type");
        GltfCameraNumericValue numeric;
        const auto& input=edit.at("value");
        if(input.is_null()) {
          if(*property!=GltfCameraProperty::PerspectiveAspectRatio && *property!=GltfCameraProperty::PerspectiveFar)
            throw std::invalid_argument("Only perspective aspect and far can be omitted");
          numeric=std::monostate{};
        } else {
          if(!input.is_number()) throw std::invalid_argument("Capture camera value must be numeric or an allowed omission");
          const double number=input.get<double>();
          if(!std::isfinite(number) || number>std::numeric_limits<float>::max() ||
              (*property==GltfCameraProperty::OrthographicNear?number<0:number<=0) ||
              (*property==GltfCameraProperty::PerspectiveYfov && number>=3.14159265358979323846))
            throw std::invalid_argument("Capture camera value is outside its supported finite range");
          numeric=number;
        }
        result.patch.values.push_back({*property,numeric});
      }
    }
    if(!result.patch.type && result.patch.values.empty()) throw std::invalid_argument("Capture cameraProjection requires an edit");
    return result;
  }
  static void nodeFields(const nlohmann::json& value,
                         std::initializer_list<const char*> fields, const char* label) {
    if (!value.is_object())
      throw std::invalid_argument(std::string("Capture ") + label + " must be an object");
    for (const auto& [name, ignored] : value.items()) {
      (void)ignored;
      if (std::none_of(fields.begin(), fields.end(), [&](const auto* field) { return name == field; }))
        throw std::invalid_argument(std::string("Unknown capture ") + label + " field: " + name);
    }
  }
  static double materialNumber(const nlohmann::json& value,
      container::geometry::gltf::GltfMaterialProperty property) {
    using P = container::geometry::gltf::GltfMaterialProperty;
    if (!value.is_number())
      throw std::invalid_argument("Capture material factor requires a numeric component");
    const double number = value.get<double>();
    if (!std::isfinite(number) || std::abs(number) > std::numeric_limits<float>::max())
      throw std::invalid_argument("Capture material factor must be finite and within binary32 range");
    bool unit = false;
    switch (property) {
    case P::BaseColor: case P::Metallic: case P::Roughness: case P::Emissive:
    case P::OcclusionStrength: case P::Transmission: case P::AttenuationColor:
    case P::Clearcoat: case P::ClearcoatRoughness: case P::SheenColor: case P::SheenRoughness:
    case P::Specular: case P::SpecularColor: case P::Iridescence: case P::Anisotropy:
    case P::DiffuseTransmission: case P::DiffuseTransmissionColor:
    case P::SpecularGlossinessDiffuse: case P::SpecularGlossinessSpecular: case P::Glossiness:
      unit = true; break;
    default: break;
    }
    if (unit && (number < 0 || number > 1))
      throw std::invalid_argument("Capture material factor/color must be in [0,1]");
    if (property == P::Ior && number != 0 && number < 1)
      throw std::invalid_argument("Capture material IOR must be zero or at least one");
    if (property == P::IridescenceIor && number < 1)
      throw std::invalid_argument("Capture material iridescence IOR must be at least one");
    if (property == P::AttenuationDistance && (number <= 0 || static_cast<float>(number) <= 0))
      throw std::invalid_argument("Capture material attenuation distance must be positive or reset with null");
    if ((property == P::AlphaCutoff || property == P::EmissiveStrength || property == P::Dispersion ||
         property == P::Thickness || property == P::IridescenceThicknessMinimum || property == P::IridescenceThicknessMaximum) && number < 0)
      throw std::invalid_argument("Capture material factor must be nonnegative");
    return number;
  }
  static CaptureResourceRetirement resourceRetirementRequest(const nlohmann::json& value) {
    using D=container::ui::ResourceCatalogDomain;using K=container::ui::ResourceCatalogKind;
    if(!value.is_object() || value.size()>5)
      throw std::invalid_argument("Capture resourceRetirement must be a bounded object");
    for(const auto& [key,ignored]:value.items()) {
      (void)ignored;
      if(key.size()>32) throw std::invalid_argument("Capture resourceRetirement field is too long");
    }
    nodeFields(value,{"domain","kind","row","expectedResourceRevision","expectedIncarnation"},"resourceRetirement");
    if(!value.contains("domain") || !value.at("domain").is_string() || !value.contains("kind") ||
        !value.at("kind").is_string() || !value.contains("row"))
      throw std::invalid_argument("Capture resourceRetirement requires explicit domain, kind and row");
    CaptureResourceRetirement result;
    const auto& domain=value.at("domain").get_ref<const std::string&>();
    const auto& kind=value.at("kind").get_ref<const std::string&>();
    if(domain=="archive") result.domain=D::Archive;
    else if(domain=="evaluated") result.domain=D::Evaluated;
    else if(domain=="runtime") result.domain=D::Runtime;
    else throw std::invalid_argument("Capture resourceRetirement domain is invalid");
    if(kind=="image") result.kind=K::Image;
    else if(kind=="texture") result.kind=K::Texture;
    else if(kind=="sampler") result.kind=K::Sampler;
    else if(kind=="runtimeTexture") result.kind=K::RuntimeTexture;
    else throw std::invalid_argument("Capture resourceRetirement kind is invalid");
    if((result.domain==D::Runtime)!=(result.kind==K::RuntimeTexture))
      throw std::invalid_argument("Capture runtime resources require the runtimeTexture kind");
    const auto integer=[&](const char* key,uint64_t minimum,uint64_t maximum) {
      const auto& number=value.at(key);
      if(!number.is_number_integer() || (number.is_number_unsigned()
          ? number.get<uint64_t>()<minimum || number.get<uint64_t>()>maximum
          : number.get<int64_t>()<0 || uint64_t(number.get<int64_t>())<minimum || uint64_t(number.get<int64_t>())>maximum))
        throw std::invalid_argument(std::string("Capture resourceRetirement ")+key+" is outside its exact integer range");
      return number.get<uint64_t>();
    };
    result.row=uint32_t(integer("row",0,999999));
    if(value.contains("expectedResourceRevision")) result.expectedResourceRevision=integer("expectedResourceRevision",1,9007199254740991ull);
    if(value.contains("expectedIncarnation")) result.expectedIncarnation=integer("expectedIncarnation",1,9007199254740991ull);
    return result;
  }
  static CaptureMaterialRetirement materialRetirementRequest(const nlohmann::json& value) {
    if (!value.is_object() || value.size() > 3)
      throw std::invalid_argument("Capture materialRetirement must be a bounded object");
    for (const auto& [key, ignored] : value.items()) {
      (void)ignored;
      if (key.size() > 32) throw std::invalid_argument("Capture materialRetirement field is too long");
    }
    nodeFields(value, {"runtimeMaterial", "sourceMaterial", "expectedAuthoredRevision"}, "materialRetirement");
    if (!value.contains("runtimeMaterial") || !value.contains("sourceMaterial"))
      throw std::invalid_argument("Capture materialRetirement requires explicit runtime/source identities");
    CaptureMaterialRetirement result;
    const auto& runtime=value.at("runtimeMaterial");
    constexpr uint64_t maximum=uint64_t(std::numeric_limits<uint32_t>::max())-1;
    if (!runtime.is_number_integer() || (runtime.is_number_unsigned()
        ? runtime.get<uint64_t>()>maximum : runtime.get<int64_t>()<0 || uint64_t(runtime.get<int64_t>())>maximum))
      throw std::invalid_argument("Capture materialRetirement runtimeMaterial must be an integer in [0,UINT32_MAX-1]");
    result.runtimeMaterial=runtime.get<uint32_t>();
    const auto& source=value.at("sourceMaterial");
    if (!source.is_number_integer() || (source.is_number_unsigned()
        ? source.get<uint64_t>()>uint64_t(std::numeric_limits<int32_t>::max())
        : source.get<int64_t>() < -1 || source.get<int64_t>()>std::numeric_limits<int32_t>::max()))
      throw std::invalid_argument("Capture materialRetirement sourceMaterial must be an integer in [-1,INT32_MAX]");
    result.sourceMaterial=source.get<int32_t>();
    if (value.contains("expectedAuthoredRevision")) {
      const auto& revision=value.at("expectedAuthoredRevision");
      if (!revision.is_number_integer() || (revision.is_number_unsigned()
          ? revision.get<uint64_t>()>9007199254740991ull
          : revision.get<int64_t>()<1 || uint64_t(revision.get<int64_t>())>9007199254740991ull) ||
          (revision.is_number_unsigned() && !revision.get<uint64_t>()))
        throw std::invalid_argument("Capture materialRetirement expectedAuthoredRevision must be an exact positive integer");
      result.expectedAuthoredRevision=revision.get<uint64_t>();
    }
    return result;
  }
  static CaptureMaterialBinding materialBindingRequest(const nlohmann::json& value) {
    using Operation = container::material::MaterialBindingEditOperation;
    if (!value.is_object() || value.size() > 6)
      throw std::invalid_argument("Capture materialBinding must be a bounded object");
    for (const auto& [key, ignored] : value.items()) {
      (void)ignored;
      if (key.size() > 32) throw std::invalid_argument("Capture materialBinding field is too long");
    }
    nodeFields(value, {"operation", "graphNode", "expectedRuntimeMaterial", "sourceMaterial", "runtimeMaterial", "name"}, "materialBinding");
    for (const auto* required : {"operation", "graphNode", "expectedRuntimeMaterial", "sourceMaterial", "runtimeMaterial"})
      if (!value.contains(required))
        throw std::invalid_argument("Capture materialBinding requires operation and explicit graph/source/runtime identities");
    const auto& operation = value.at("operation");
    if (!operation.is_string()) throw std::invalid_argument("Capture materialBinding operation must be assign or makeUnique");
    const auto& operationName = operation.get_ref<const std::string&>();
    CaptureMaterialBinding request;
    if (operationName == "assign") request.operation = Operation::Assign;
    else if (operationName == "makeUnique") request.operation = Operation::MakeUnique;
    else throw std::invalid_argument("Capture materialBinding operation must be assign or makeUnique");
    const auto uintIdentity = [&](const char* field) {
      const auto& id = value.at(field);
      constexpr uint64_t maximum = uint64_t(std::numeric_limits<uint32_t>::max()) - 1;
      if (!id.is_number_integer() || (id.is_number_unsigned()
          ? id.get<uint64_t>() > maximum : id.get<int64_t>() < 0 || uint64_t(id.get<int64_t>()) > maximum))
        throw std::invalid_argument("Capture materialBinding graph/runtime identities must be integers in [0,UINT32_MAX-1]");
      return id.get<uint32_t>();
    };
    request.graphNode = uintIdentity("graphNode");
    request.expectedRuntimeMaterial = uintIdentity("expectedRuntimeMaterial");
    request.runtimeMaterial = uintIdentity("runtimeMaterial");
    const auto& source = value.at("sourceMaterial");
    if (!source.is_number_integer() || (source.is_number_unsigned()
        ? source.get<uint64_t>() > uint64_t(std::numeric_limits<int32_t>::max())
        : source.get<int64_t>() < -1 || source.get<int64_t>() > std::numeric_limits<int32_t>::max()))
      throw std::invalid_argument("Capture materialBinding sourceMaterial must be an integer in [-1,INT32_MAX]");
    request.sourceMaterial = source.get<int32_t>();
    if (value.contains("name")) {
      const auto& name = value.at("name");
      if (request.operation != Operation::MakeUnique || !name.is_string() || name.get_ref<const std::string&>().size() > 4096)
        throw std::invalid_argument("Capture materialBinding name is a bounded string for makeUnique only");
      const auto& text = name.get_ref<const std::string&>();
      container::scene::ValidateSceneNodeName(text);
      request.name = text;
    }
    return request;
  }
  static CaptureMaterialTextureEdit materialTextureEditRequest(const nlohmann::json& value) {
    using namespace container::geometry::gltf;
    using Operation=container::material::MaterialTextureEditOperation;
    const auto boundedFields=[&](const nlohmann::json& object,std::initializer_list<const char*> fields,const char* label) {
      if(!object.is_object() || object.size()>fields.size()) throw std::invalid_argument("Capture texture edit object exceeds its field limit");
      for(const auto& [key,ignored]:object.items()) {
        (void)ignored;if(key.size()>32) throw std::invalid_argument("Capture texture edit field is too long");
      }
      nodeFields(object,fields,label);
    };
    boundedFields(value,{"sourceMaterial","slot","operation","path","sourceTexture","sampler","mapping","expectedResourceRevision"},"materialTextureEdit");
    for(const auto* field:{"sourceMaterial","slot","operation"})
      if(!value.contains(field)) throw std::invalid_argument("Capture texture edit requires sourceMaterial, slot and operation");
    const auto nonnegative=[&](const nlohmann::json& number,uint64_t maximum,const char* label) {
      if(!number.is_number_integer() || (number.is_number_unsigned()?number.get<uint64_t>()>maximum:
          number.get<int64_t>()<0 || uint64_t(number.get<int64_t>())>maximum))
        throw std::invalid_argument(label);
      return number.get<uint64_t>();
    };
    CaptureMaterialTextureEdit request;
    request.sourceMaterial=int32_t(nonnegative(value.at("sourceMaterial"),uint64_t(INT32_MAX),"Capture texture sourceMaterial must be an integer in [0,INT32_MAX]"));
    const auto& slot=value.at("slot");
    if(!slot.is_string() || slot.get_ref<const std::string&>().size()>32) throw std::invalid_argument("Capture texture slot must be a bounded name");
    const auto selected=container::material::FindMaterialTextureSlot(slot.get_ref<const std::string&>());
    if(!selected) throw std::invalid_argument("Capture texture slot is unsupported");request.slot=*selected;
    const auto& operation=value.at("operation");
    if(!operation.is_string() || operation.get_ref<const std::string&>().size()>16) throw std::invalid_argument("Capture texture operation must be a bounded name");
    const auto& name=operation.get_ref<const std::string&>();
    if(name=="importFile") request.operation=Operation::ImportFile;
    else if(name=="bindExisting") request.operation=Operation::BindExisting;
    else if(name=="clear") request.operation=Operation::Clear;
    else if(name=="editMapping") request.operation=Operation::EditMapping;
    else if(name=="editSampler") request.operation=Operation::EditSampler;
    else throw std::invalid_argument("Capture texture operation is unsupported");
    if(value.contains("path")) {
      const auto& path=value.at("path");
      if(!path.is_string()) throw std::invalid_argument("Capture texture path must be a string");
      const auto& text=path.get_ref<const std::string&>();
      if(text.empty() || text.size()>65536 || text.find('\0')!=std::string::npos)
        throw std::invalid_argument("Capture texture path must be a bounded nonempty path");
      request.path=text;
    }
    if(value.contains("sourceTexture")) request.sourceTexture=uint32_t(nonnegative(value.at("sourceTexture"),uint64_t(INT32_MAX),
        "Capture sourceTexture must be an integer in [0,INT32_MAX]"));
    if(value.contains("expectedResourceRevision")) {
      request.expectedResourceRevision=nonnegative(value.at("expectedResourceRevision"),UINT64_MAX,
          "Capture texture resource revision must be a positive integer");
      if(!*request.expectedResourceRevision) throw std::invalid_argument("Capture texture resource revision must be a positive integer");
    }
    if(value.contains("sampler")) {
      const auto& sampler=value.at("sampler");boundedFields(sampler,{"minFilter","magFilter","wrapS","wrapT"},"texture sampler");
      container::material::MaterialTextureSamplerDescription description;
      for(const auto& [key,number]:sampler.items()) {
        if(!number.is_number_integer() || (number.is_number_unsigned()?number.get<uint64_t>()>uint64_t(INT32_MAX):
            number.get<int64_t>()< -1 || number.get<int64_t>()>INT32_MAX))
          throw std::invalid_argument("Capture texture sampler fields must be exact integer enums");
        const int integer=number.get<int>();
        if(key=="minFilter") description.minFilter=integer;else if(key=="magFilter") description.magFilter=integer;
        else if(key=="wrapS") description.wrapS=integer;else description.wrapT=integer;
      }
      container::material::ValidateMaterialTextureSampler(description);request.sampler=description;
    }
    if(value.contains("mapping")) {
      const auto& mapping=value.at("mapping");boundedFields(mapping,{"texCoord","channel","offset","scale","rotation"},"texture mapping");
      if(mapping.empty()) throw std::invalid_argument("Capture texture mapping requires at least one field");
      const auto finite=[](const nlohmann::json& number) {
        if(!number.is_number() || !std::isfinite(number.get<double>()) || std::abs(number.get<double>())>std::numeric_limits<float>::max())
          throw std::invalid_argument("Capture texture mapping must be finite and representable in binary32");
        return number.get<double>();
      };
      for(const auto& [key,item]:mapping.items()) {
        if(key=="texCoord" || key=="channel") {
          auto& field=key=="texCoord"?request.mapping.texCoord:request.mapping.channel;
          if(item.is_null()) field=std::monostate{};
          else field=uint32_t(nonnegative(item,key=="texCoord"?1:4,"Capture texture mapping enum is invalid"));
        } else if(key=="rotation") {
          if(item.is_null()) request.mapping.rotation=std::monostate{};else request.mapping.rotation=finite(item);
        } else {
          auto& field=key=="offset"?request.mapping.offset:request.mapping.scale;
          if(item.is_null()) field=std::monostate{};
          else {
            if(!item.is_array() || item.size()!=2) throw std::invalid_argument("Capture texture mapping vectors require two components");
            field=std::array<double,2>{finite(item[0]),finite(item[1])};
          }
        }
      }
    }
    container::material::ValidateMaterialTextureEditRequest(request);return request;
  }
  static CaptureMaterialEdit materialEditRequest(const nlohmann::json& value) {
    using namespace container::geometry::gltf;
    using Phase = container::material::MaterialEditGesturePhase;
    constexpr uint64_t maxGesture = (uint64_t(1) << 53) - 1;
    if (!value.is_object() || value.size() > 8)
      throw std::invalid_argument("Capture materialEdit must be a bounded object");
    for (const auto& [key, ignored] : value.items()) {
      (void)ignored;
      if (key.size() > 32) throw std::invalid_argument("Capture materialEdit field is too long");
    }
    nodeFields(value, {"sourceMaterial", "values", "extensions", "alphaMode", "doubleSided", "name", "gestureId", "gesturePhase"}, "materialEdit");
    if (!value.contains("sourceMaterial"))
      throw std::invalid_argument("Capture materialEdit requires sourceMaterial");
    const auto& source = value.at("sourceMaterial");
    if (!source.is_number_integer() || (source.is_number_unsigned()
        ? source.get<uint64_t>() > uint64_t(std::numeric_limits<int32_t>::max())
        : source.get<int64_t>() < 0 || source.get<int64_t>() > std::numeric_limits<int32_t>::max()))
      throw std::invalid_argument("Capture sourceMaterial must be an integer in [0,INT32_MAX]");
    CaptureMaterialEdit request;
    request.sourceMaterial = source.get<int32_t>();
    if (value.contains("gesturePhase")) {
      const auto& phase = value.at("gesturePhase");
      if (!phase.is_string() || phase.get_ref<const std::string&>().size() > 6)
        throw std::invalid_argument("Capture material gesture phase is invalid");
      const auto& name = phase.get_ref<const std::string&>();
      if (name == "commit") request.phase = Phase::Commit;
      else if (name == "begin") request.phase = Phase::Begin;
      else if (name == "update") request.phase = Phase::Update;
      else if (name == "end") request.phase = Phase::End;
      else if (name == "cancel") request.phase = Phase::Cancel;
      else throw std::invalid_argument("Capture material gesture phase is unsupported");
    }
    if (value.contains("gestureId")) {
      const auto& id = value.at("gestureId");
      if (!id.is_number_integer() || (id.is_number_unsigned()
          ? id.get<uint64_t>() == 0 || id.get<uint64_t>() > maxGesture
          : id.get<int64_t>() <= 0 || uint64_t(id.get<int64_t>()) > maxGesture))
        throw std::invalid_argument("Capture material gestureId must be an integer in [1,2^53-1]");
      request.gestureId = id.get<uint64_t>();
    }
    if ((request.phase == Phase::Commit) == request.gestureId.has_value())
      throw std::invalid_argument("Capture material begin/update/end/cancel requires gestureId; commit forbids it");

    const size_t numericCount = value.contains("values") ? value.at("values").size() : 0;
    const size_t extensionCount = value.contains("extensions") ? value.at("extensions").size() : 0;
    if ((value.contains("values") && !value.at("values").is_array()) || numericCount > size_t(GltfMaterialProperty::Count) ||
        (value.contains("extensions") && !value.at("extensions").is_array()) || extensionCount > size_t(GltfMaterialExtension::Count))
      throw std::invalid_argument("Capture material patch arrays exceed their typed operation limits");
    const size_t metadataCount = size_t(value.contains("alphaMode")) + size_t(value.contains("doubleSided")) + size_t(value.contains("name"));
    if (request.phase == Phase::Cancel && (value.contains("values") || value.contains("extensions") || metadataCount))
      throw std::invalid_argument("Capture material cancel forbids patch fields");
    if (request.phase != Phase::End && request.phase != Phase::Cancel && numericCount + extensionCount + metadataCount == 0)
      throw std::invalid_argument("Capture material commit/begin/update requires at least one patch operation");

    std::array<bool, size_t(GltfMaterialProperty::Count)> properties{};
    std::array<bool, size_t(GltfMaterialExtension::Count)> extensions{}, disabled{};
    request.patch.values.reserve(numericCount);
    if (value.contains("values")) for (const auto& edit : value.at("values")) {
      if (!edit.is_object() || edit.size() != 2 || !edit.contains("property") || !edit.contains("value"))
        throw std::invalid_argument("Capture material numeric edit requires exactly property and value");
      const auto& key = edit.at("property");
      if (!key.is_string() || key.get_ref<const std::string&>().size() > 256)
        throw std::invalid_argument("Capture material property path is invalid");
      const auto registry = GltfMaterialPropertyRegistry();
      const auto property = std::find_if(registry.begin(), registry.end(), [&](const auto& entry) { return entry.path == key.get_ref<const std::string&>(); });
      if (property == registry.end()) throw std::invalid_argument("Capture material property is not in the numeric registry");
      if (properties[size_t(property->property)]) throw std::invalid_argument("Capture material property is duplicated");
      properties[size_t(property->property)] = true;
      GltfMaterialNumericValue number;
      const auto& input = edit.at("value");
      if (!input.is_null()) {
        if (property->components == 1) number = materialNumber(input, property->property);
        else {
          if (!input.is_array() || input.size() != property->components)
            throw std::invalid_argument("Capture material vector has the wrong component count");
          if (property->components == 3) {
            std::array<double, 3> components;
            for (size_t i = 0; i < components.size(); ++i) components[i] = materialNumber(input[i], property->property);
            number = components;
          } else {
            std::array<double, 4> components;
            for (size_t i = 0; i < components.size(); ++i) components[i] = materialNumber(input[i], property->property);
            number = components;
          }
        }
      }
      request.patch.values.push_back({property->property, number});
    }
    request.patch.extensions.reserve(extensionCount);
    if (value.contains("extensions")) for (const auto& edit : value.at("extensions")) {
      if (!edit.is_object() || edit.size() != 2 || !edit.contains("extension") || !edit.contains("enabled") ||
          !edit.at("extension").is_string() || !edit.at("enabled").is_boolean())
        throw std::invalid_argument("Capture material extension edit requires exactly extension string and enabled boolean");
      const auto& name = edit.at("extension").get_ref<const std::string&>();
      if (name.size() > 64) throw std::invalid_argument("Capture material extension name is too long");
      size_t extension = 0;
      while (extension < size_t(GltfMaterialExtension::Count) && GltfMaterialExtensionName(GltfMaterialExtension(extension)) != name) ++extension;
      if (extension == size_t(GltfMaterialExtension::Count)) throw std::invalid_argument("Capture material extension is unsupported");
      if (extensions[extension]) throw std::invalid_argument("Capture material extension is duplicated");
      extensions[extension] = true;
      disabled[extension] = !edit.at("enabled").get<bool>();
      request.patch.extensions.push_back({GltfMaterialExtension(extension), !disabled[extension]});
    }
    for (const auto& property : GltfMaterialPropertyRegistry())
      if (properties[size_t(property.property)] && property.extension && disabled[size_t(*property.extension)])
        throw std::invalid_argument("Capture material numeric edit conflicts with disabling its extension");
    if (value.contains("alphaMode")) {
      const auto& mode = value.at("alphaMode");
      if (!mode.is_string()) throw std::invalid_argument("Capture material alphaMode must be a string");
      const auto& name = mode.get_ref<const std::string&>();
      if (name == "OPAQUE") request.patch.alphaMode = GltfMaterialAlphaMode::Opaque;
      else if (name == "MASK") request.patch.alphaMode = GltfMaterialAlphaMode::Mask;
      else if (name == "BLEND") request.patch.alphaMode = GltfMaterialAlphaMode::Blend;
      else throw std::invalid_argument("Capture material alphaMode is unsupported");
    }
    if (value.contains("doubleSided")) {
      if (!value.at("doubleSided").is_boolean()) throw std::invalid_argument("Capture material doubleSided must be a boolean");
      request.patch.doubleSided = value.at("doubleSided").get<bool>();
    }
    if (value.contains("name")) {
      const auto& name = value.at("name");
      if (!name.is_string() || name.get_ref<const std::string&>().size() > 4096)
        throw std::invalid_argument("Capture material name must be a bounded string");
      const auto& text = name.get_ref<const std::string&>();
      container::scene::ValidateSceneNodeName(text);
      request.patch.name = text;
    }
    return request;
  }
  static CaptureNodeReference nodeReference(const nlohmann::json& value) {
    nodeFields(value, {"graphNode", "sourceNode"}, "node reference");
    if (value.contains("graphNode") == value.contains("sourceNode"))
      throw std::invalid_argument("Capture node reference requires exactly one graphNode or sourceNode");
    CaptureNodeReference reference;
    if (value.contains("graphNode")) {
      const auto& node = value.at("graphNode");
      constexpr uint64_t maximum = uint64_t(std::numeric_limits<uint32_t>::max()) - 1;
      if (!node.is_number_integer() ||
          (node.is_number_unsigned() ? node.get<uint64_t>() > maximum :
           (node.get<int64_t>() < 0 || uint64_t(node.get<int64_t>()) > maximum)))
        throw std::invalid_argument("Capture graphNode reference must be an integer in [0,UINT32_MAX-1]");
      reference.graphNode = node.get<uint32_t>();
    } else {
      const auto& node = value.at("sourceNode");
      if (!node.is_number_integer() ||
          (node.is_number_unsigned() ? node.get<uint64_t>() > uint64_t(std::numeric_limits<int>::max()) :
           (node.get<int64_t>() < 0 || node.get<int64_t>() > std::numeric_limits<int>::max())))
        throw std::invalid_argument("Capture sourceNode reference must be an integer in [0,INT_MAX]");
      reference.sourceNode = node.get<int>();
    }
    return reference;
  }
  static CaptureNodeReparent nodeReparentRequest(const nlohmann::json& value) {
    nodeFields(value, {"node", "parent"}, "nodeReparent");
    if (!value.contains("node") || !value.contains("parent"))
      throw std::invalid_argument("Capture nodeReparent requires node and parent references");
    return {nodeReference(value.at("node")), nodeReference(value.at("parent"))};
  }
  static CaptureNodeTransform nodeTransformRequest(const nlohmann::json& value) {
    nodeFields(value, {"node", "matrix"}, "nodeTransform");
    if (!value.contains("node") || !value.contains("matrix"))
      throw std::invalid_argument("Capture nodeTransform requires a node reference and matrix");
    CaptureNodeTransform request;
    request.node = nodeReference(value.at("node"));
    const auto& matrix = value.at("matrix");
    if (!matrix.is_array() || matrix.size() != 16)
      throw std::invalid_argument("Capture nodeTransform matrix requires 16 column-major numbers");
    for (size_t index = 0; index < 16; ++index) {
      const auto& value = matrix[index];
      if (!value.is_number() || !std::isfinite(value.get<double>()) ||
          std::abs(value.get<double>()) > std::numeric_limits<float>::max())
        throw std::invalid_argument("Capture nodeTransform matrix must be finite and representable in binary32");
      const double number = value.get<double>();
      if (((index == 3 || index == 7 || index == 11) && number != 0) || (index == 15 && number != 1))
        throw std::invalid_argument("Capture nodeTransform matrix must be column-major affine");
      request.matrix[index / 4][index % 4] = static_cast<float>(number);
    }
    return request;
  }
  static CaptureAddPrimitive addPrimitiveRequest(const nlohmann::json& value) {
    nodeFields(value, {"kind", "name", "parent"}, "addPrimitive");
    if (!value.contains("kind") || !value.at("kind").is_string() ||
        !value.contains("name") || !value.at("name").is_string())
      throw std::invalid_argument("Capture addPrimitive requires kind and name strings");
    CaptureAddPrimitive request;
    request.kind = value.at("kind").get<std::string>();
    if (request.kind != "quad" && request.kind != "plane" && request.kind != "cube" && request.kind != "sphere" &&
        request.kind != "triangular-prism" && request.kind != "pyramid" && request.kind != "torus")
      throw std::invalid_argument("Capture addPrimitive kind is unsupported");
    request.name = value.at("name").get<std::string>();
    container::scene::ValidateSceneNodeName(request.name);
    if (value.contains("parent")) request.parent = nodeReference(value.at("parent"));
    return request;
  }
  static CaptureNodeAuthoring nodeAuthoringRequest(const nlohmann::json& value) {
    if (!value.is_object())
      throw std::invalid_argument("Capture nodeAuthoring must be an object");
    for (const auto& [field, ignored] : value.items()) {
      (void)ignored;
      if (field != "action" && field != "graphNode" && field != "sourceNode" && field != "name")
        throw std::invalid_argument("Unknown capture nodeAuthoring field: " + field);
    }
    if (!value.contains("action") || !value.at("action").is_string())
      throw std::invalid_argument("Capture nodeAuthoring requires an action");
    CaptureNodeAuthoring request;
    request.action = value.at("action").get<std::string>();
    if (request.action != "rename" && request.action != "create-empty" &&
        request.action != "delete-subtree" && request.action != "duplicate-subtree" &&
        request.action != "make-editable")
      throw std::invalid_argument("Unknown capture nodeAuthoring action '" + request.action + "'");
    if (value.contains("graphNode") == value.contains("sourceNode"))
      throw std::invalid_argument("Capture nodeAuthoring requires exactly one graphNode or sourceNode");
    if (value.contains("graphNode")) {
      const auto& node = value.at("graphNode");
      constexpr uint64_t maximum = uint64_t(std::numeric_limits<uint32_t>::max()) - 1;
      if (!node.is_number_integer() ||
          (node.is_number_unsigned() ? node.get<uint64_t>() > maximum :
           (node.get<int64_t>() < 0 || uint64_t(node.get<int64_t>()) > maximum)))
        throw std::invalid_argument("Capture nodeAuthoring graphNode must be an integer in [0,UINT32_MAX-1]");
      request.graphNode = node.get<uint32_t>();
    } else {
      const auto& node = value.at("sourceNode");
      if (!node.is_number_integer() ||
          (node.is_number_unsigned() ? node.get<uint64_t>() > uint64_t(std::numeric_limits<int>::max()) :
           (node.get<int64_t>() < 0 || node.get<int64_t>() > std::numeric_limits<int>::max())))
        throw std::invalid_argument("Capture nodeAuthoring sourceNode must be an integer in [0,INT_MAX]");
      request.sourceNode = node.get<int>();
    }
    if (request.action == "delete-subtree" || request.action == "make-editable") {
      if (value.contains("name"))
        throw std::invalid_argument("Capture " + request.action + " requires the name field omitted");
      return request;
    }
    if (request.action == "duplicate-subtree" && !value.contains("name")) return request;
    if (!value.contains("name") || !value.at("name").is_string())
      throw std::invalid_argument("Capture nodeAuthoring requires a name string");
    const auto& name = value.at("name").get_ref<const std::string&>();
    container::scene::ValidateSceneNodeName(name);
    request.name = name;
    return request;
  }

  static CaptureMergeRequest mergeRequest(const nlohmann::json& value) {
    if (!value.is_object())
      throw std::invalid_argument("Capture merge must be an object");
    for (const auto& [name, ignored] : value.items()) {
      (void)ignored;
      if (name != "path" && name != "parentName" && name != "incomingScene" && name != "placementMatrix")
        throw std::invalid_argument("Unknown capture merge field: " + name);
    }
    if (!value.contains("path") || !value.at("path").is_string() || value.at("path").get_ref<const std::string&>().empty())
      throw std::invalid_argument("Capture merge requires a nonempty path string");
    CaptureMergeRequest request;
    request.path = value.at("path").get<std::string>();
    if (value.contains("parentName")) {
      const auto& parent = value.at("parentName");
      if (!parent.is_string() || parent.get_ref<const std::string&>().empty())
        throw std::invalid_argument("Capture merge parentName must be a nonempty string");
      request.parentName = parent.get<std::string>();
    }
    if (value.contains("incomingScene")) {
      const auto& scene = value.at("incomingScene");
      if (!scene.is_number_integer() ||
          (scene.is_number_unsigned() ? scene.get<uint64_t>() > uint64_t(std::numeric_limits<int>::max()) :
           (scene.get<int64_t>() < 0 || scene.get<int64_t>() > std::numeric_limits<int>::max())))
        throw std::invalid_argument("Capture merge incomingScene must be an integer in [0,INT_MAX]");
      request.incomingScene = scene.get<int>();
    }
    if (value.contains("placementMatrix")) {
      const auto& encoded = value.at("placementMatrix");
      if (!encoded.is_array() || encoded.size() != 16u)
        throw std::invalid_argument("Capture merge placementMatrix needs sixteen column-major numbers");
      std::array<double, 16> matrix{};
      for (size_t i = 0; i < matrix.size(); ++i) {
        if (!encoded[i].is_number())
          throw std::invalid_argument("Capture merge placementMatrix needs sixteen numeric values");
        matrix[i] = encoded[i].get<double>();
      }
      container::geometry::gltf::ValidateGltfMergePlacement(matrix);
      request.placementMatrix = matrix;
    }
    return request;
  }
  static container::physics::PhysicsBodySettings bodySettings(const nlohmann::json &value) {
    knownFields(value, {"mass", "friction", "restitution", "gravityFactor", "useCCD",
                         "linearVelocity", "angularVelocity"});
    container::physics::PhysicsBodySettings settings;
    for (const auto &[name, target] : std::initializer_list<std::pair<const char *, float *>>{
             {"mass", &settings.mass}, {"friction", &settings.friction},
             {"restitution", &settings.restitution}, {"gravityFactor", &settings.gravityFactor}})
      if (value.contains(name)) *target = finiteNumber(value.at(name));
    if (!(settings.mass > 0) || !std::isfinite(1.0f / settings.mass) ||
        settings.friction < 0 || settings.restitution < 0 || settings.restitution > 1)
      throw std::invalid_argument("Invalid capture physics body mass or material");
    if (value.contains("useCCD")) {
      if (!value.at("useCCD").is_boolean())
        throw std::invalid_argument("Capture useCCD must be boolean");
      settings.useCCD = value.at("useCCD").get<bool>();
    }
    const auto velocity = [&](const char *name, glm::vec3 &target) {
      if (!value.contains(name)) return;
      const auto &vector = value.at(name);
      if (!vector.is_array() || vector.size() != 3u)
        throw std::invalid_argument("Capture body velocities need three finite numbers");
      for (int axis = 0; axis < 3; ++axis) target[axis] = finiteNumber(vector[axis]);
    };
    velocity("linearVelocity", settings.linearVelocity);
    velocity("angularVelocity", settings.angularVelocity);
    return settings;
  }
  static float finiteNumber(const nlohmann::json &value) {
    if (!value.is_number())
      throw std::invalid_argument("Capture physics values must be numbers");
    const double number = value.get<double>();
    if (!std::isfinite(number) || std::abs(number) > std::numeric_limits<float>::max())
      throw std::invalid_argument("Capture physics value must be finite and representable");
    return static_cast<float>(number);
  }
  static void knownFields(const nlohmann::json &object,
                          std::initializer_list<const char *> fields) {
    if (!object.is_object())
      throw std::invalid_argument("Capture physics commands must be objects");
    for (const auto &[name, ignored] : object.items()) {
      (void)ignored;
      if (std::none_of(fields.begin(), fields.end(),
                       [&](const char *field) { return name == field; }))
        throw std::invalid_argument("Unknown capture physics field");
    }
  }
  static CapturePhysicsJointBody jointBody(const nlohmann::json &value, bool allowWorld) {
    knownFields(value, {"node", "bimObject", "world"});
    if (value.size() != 1u)
      throw std::invalid_argument("Capture joint body requires exactly one selector");
    CapturePhysicsJointBody result;
    if (value.contains("world")) {
      if (!allowWorld || !value.at("world").is_boolean() || !value.at("world").get<bool>())
        throw std::invalid_argument("Only joint body A may select the fixed world with world=true");
      result.world = true;
    } else {
      const auto &index = value.at(value.contains("node") ? "node" : "bimObject");
      const double number = index.is_number() ? index.get<double>() : -1.0;
      if (!std::isfinite(number) || number < 0.0 ||
          number > double(std::numeric_limits<uint32_t>::max()) || std::floor(number) != number)
        throw std::invalid_argument("Capture joint body selector must be a uint32 index");
      if (value.contains("node")) result.node = index.get<uint32_t>();
      else result.bimObject = index.get<uint32_t>();
    }
    return result;
  }
  static CapturePhysicsJointCreate jointCreate(const nlohmann::json &value) {
    using namespace container::physics;
    knownFields(value, {"type", "bodyA", "bodyB", "anchor", "axis", "limits", "motor", "collideConnected"});
    for (const char *field : {"type", "bodyA", "bodyB", "anchor"})
      if (!value.contains(field))
        throw std::invalid_argument("Capture joint creation is missing a required field");
    const auto &type = value.at("type");
    if (!type.is_string() || (type != "fixed" && type != "hinge" && type != "slider"))
      throw std::invalid_argument("Unsupported capture physics joint type");
    CapturePhysicsJointCreate result;
    result.settings.type = type == "fixed" ? PhysicsJointType::Fixed :
                           type == "hinge" ? PhysicsJointType::Hinge : PhysicsJointType::Slider;
    result.bodyA = jointBody(value.at("bodyA"), true);
    result.bodyB = jointBody(value.at("bodyB"), false);
    if ((result.bodyA.node && result.bodyA.node == result.bodyB.node) ||
        (result.bodyA.bimObject && result.bodyA.bimObject == result.bodyB.bimObject))
      throw std::invalid_argument("Capture joint endpoints must be distinct bodies");
    auto vector = [&](const nlohmann::json &encoded) {
      if (!encoded.is_array() || encoded.size() != 3u)
        throw std::invalid_argument("Capture joint vectors need three finite numbers");
      return glm::vec3{finiteNumber(encoded[0]), finiteNumber(encoded[1]), finiteNumber(encoded[2])};
    };
    const auto &anchor = value.at("anchor");
    if (!anchor.is_array() || anchor.size() != 3u)
      throw std::invalid_argument("Capture joint anchor needs three finite numbers");
    for (int axis = 0; axis < 3; ++axis) {
      if (!anchor[axis].is_number())
        throw std::invalid_argument("Capture joint anchor components must be numbers");
      const double coordinate = anchor[axis].get<double>();
      if (!std::isfinite(coordinate) ||
          std::abs(coordinate) > double(std::numeric_limits<float>::max()) * .25)
        throw std::invalid_argument("Capture joint anchor must be finite and representable");
      result.settings.anchor[axis] = coordinate;
    }
    if (value.contains("axis")) result.settings.axis = vector(value.at("axis"));
    // Scale before normalization so a valid finite axis cannot overflow its
    // squared length, nor can a tiny axis collapse from squared underflow.
    const float scale = std::max({std::abs(result.settings.axis.x), std::abs(result.settings.axis.y),
                                  std::abs(result.settings.axis.z)});
    if (!(scale > 0.0f))
      throw std::invalid_argument("Capture joint axis must be nonzero");
    result.settings.axis = glm::normalize(result.settings.axis / scale);
    const auto basis = std::abs(result.settings.axis.x) < .8f ? glm::vec3{1, 0, 0} : glm::vec3{0, 1, 0};
    result.settings.normal = glm::normalize(glm::cross(result.settings.axis, basis));
    if (value.contains("limits")) {
      const auto &limits = value.at("limits");
      if (result.settings.type == PhysicsJointType::Fixed || !limits.is_array() || limits.size() != 2u)
        throw std::invalid_argument("Capture hinge/slider limits require two numbers");
      result.settings.lowerLimit = finiteNumber(limits[0]);
      result.settings.upperLimit = finiteNumber(limits[1]);
      if (result.settings.lowerLimit > 0 || result.settings.upperLimit < 0 ||
          result.settings.lowerLimit >= result.settings.upperLimit ||
          (result.settings.type == PhysicsJointType::Hinge &&
           (result.settings.lowerLimit < -3.14159265358979323846f ||
            result.settings.upperLimit > 3.14159265358979323846f)))
        throw std::invalid_argument("Capture joint limits must contain zero and hinge limits stay within pi");
      result.settings.limitsEnabled = true;
    }
    if (value.contains("motor")) {
      const auto &motor = value.at("motor");
      knownFields(motor, {"mode", "targetVelocity", "targetPosition", "maxForceOrTorque", "frequencyHz", "dampingRatio"});
      if (!motor.contains("mode") || !motor.at("mode").is_string())
        throw std::invalid_argument("Capture joint motor requires an explicit mode");
      const auto &mode = motor.at("mode");
      if (mode != "off" && mode != "velocity" && mode != "position")
        throw std::invalid_argument("Unsupported capture physics joint motor mode");
      auto &settings = result.settings.motor;
      settings.mode = mode == "off" ? PhysicsJointMotorMode::Off :
                      mode == "velocity" ? PhysicsJointMotorMode::Velocity : PhysicsJointMotorMode::Position;
      if (result.settings.type == PhysicsJointType::Fixed && settings.mode != PhysicsJointMotorMode::Off)
        throw std::invalid_argument("Fixed capture joints have no motor");
      for (const auto &[name, target] : std::initializer_list<std::pair<const char *, float *>>{
               {"targetVelocity", &settings.targetVelocity}, {"targetPosition", &settings.targetPosition},
               {"maxForceOrTorque", &settings.maxForceOrTorque}, {"frequencyHz", &settings.frequencyHz},
               {"dampingRatio", &settings.dampingRatio}})
        if (motor.contains(name)) *target = finiteNumber(motor.at(name));
      if (settings.maxForceOrTorque < 0 || settings.frequencyHz <= 0 || settings.dampingRatio < 0)
        throw std::invalid_argument("Invalid capture joint motor force/spring settings");
    }
    if (value.contains("collideConnected")) {
      if (!value.at("collideConnected").is_boolean())
        throw std::invalid_argument("Capture collideConnected must be a boolean");
      result.settings.collideConnected = value.at("collideConnected").get<bool>();
    }
    return result;
  }
  template <class T> struct Key {
    uint32_t frame;
    T value;
  };
  std::vector<Key<CaptureCamera>> cameras_;
  std::vector<Key<glm::vec3>> objects_, scales_, bim_;
  std::vector<CaptureEvent> events_;
  uint32_t objectNode_{0};
  std::string objectName_{};
  void checkFrame(uint32_t frame) const {
    if (frame == 0 || frame > frameCount)
      throw std::invalid_argument("Temporal capture frame outside sequence");
  }
  static glm::vec3 vec3(const nlohmann::json &json) {
    if (!json.is_array() || json.size() != 3)
      throw std::invalid_argument("Capture vector requires three components");
    glm::vec3 result(json[0].get<float>(), json[1].get<float>(),
                     json[2].get<float>());
    if (!std::isfinite(result.x) || !std::isfinite(result.y) ||
        !std::isfinite(result.z))
      throw std::invalid_argument("Capture vector must be finite");
    return result;
  }
  template <class T> static void validateOrder(const std::vector<T> &values) {
    for (size_t i = 1; i < values.size(); ++i)
      if (values[i].frame <= values[i - 1].frame)
        throw std::invalid_argument(
            "Capture keyframes/events must have increasing unique frames");
  }
  void readTranslations(const nlohmann::json &json, const char *name,
                        std::vector<Key<glm::vec3>> &keys) {
    if (!json.contains(name))
      return;
    for (const auto &key : json.at(name)) {
      uint32_t frame = key.at("frame").get<uint32_t>();
      checkFrame(frame);
      keys.push_back({frame, vec3(key.at("translation"))});
    }
    validateOrder(keys);
  }
  void readScales(const nlohmann::json &json) {
    if (!json.contains("objectScale"))
      return;
    const auto &track = json.at("objectScale");
    if (!track.is_array())
      throw std::invalid_argument("Capture objectScale must be a keyframe array");
    for (const auto &key : track) {
      knownFields(key, {"frame", "scale"});
      if (!key.contains("frame") || !key.contains("scale") || !key.at("frame").is_number())
        throw std::invalid_argument("Capture scale keyframe requires a frame and scale");
      const double encodedFrame = key.at("frame").get<double>();
      if (!std::isfinite(encodedFrame) || std::floor(encodedFrame) != encodedFrame ||
          encodedFrame < 1 || encodedFrame > frameCount)
        throw std::invalid_argument("Capture scale keyframe frame is outside the sequence");
      const auto &encoded = key.at("scale");
      if (!encoded.is_array() || encoded.size() != 3)
        throw std::invalid_argument("Capture scale requires three nonzero finite numbers");
      glm::vec3 scale;
      for (int axis = 0; axis < 3; ++axis) {
        scale[axis] = finiteNumber(encoded[axis]);
        if (scale[axis] == 0.0f)
          throw std::invalid_argument("Capture scale components must be nonzero");
        if (!scales_.empty() && std::signbit(scale[axis]) != std::signbit(scales_.back().value[axis]))
          throw std::invalid_argument("Capture scale keyframes cannot change an axis sign");
      }
      scales_.push_back({static_cast<uint32_t>(encodedFrame), scale});
    }
    validateOrder(scales_);
  }
  static glm::vec3 mixVector(const glm::vec3 &a, const glm::vec3 &b, float t) {
    glm::vec3 result;
    for (int axis = 0; axis < 3; ++axis)
      // Weighted sums of identical floats can round differently as t changes.
      // Preserve stationary components so capture does not report false motion.
      result[axis] = a[axis] == b[axis] ? a[axis] : glm::mix(a[axis], b[axis], t);
    return result;
  }
  template <class T, class Mix>
  static T interpolate(const std::vector<Key<T>> &keys, uint32_t frame,
                       Mix mix) {
    if (frame <= keys.front().frame)
      return keys.front().value;
    for (size_t i = 1; i < keys.size(); ++i)
      if (frame <= keys[i].frame) {
        if (frame == keys[i].frame)
          return keys[i].value;
        const float t = float(frame - keys[i - 1].frame) /
                        float(keys[i].frame - keys[i - 1].frame);
        return mix(keys[i - 1].value, keys[i].value, t);
      }
    return keys.back().value;
  }
};
} // namespace container::temporal
