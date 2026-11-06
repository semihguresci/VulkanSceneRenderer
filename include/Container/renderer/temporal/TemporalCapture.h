#pragma once

#include <Container/renderer/lighting/LightEditTypes.h>

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace container::temporal {

struct CaptureCamera {
  glm::vec3 position{}, target{};
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
struct CaptureNodeEdit {
  enum class Operation { Select, Transform, Visibility, Reparent, Begin, Update, End, Cancel };
  Operation operation{};
  uint32_t sourceNode{};
  std::optional<uint32_t> graphNode;
  std::optional<uint32_t> parent;
  glm::mat4 matrix{1};
  bool visible{};
  CaptureLightGizmo::Tool tool{CaptureLightGizmo::Tool::Translate};
  CaptureLightGizmo::Space space{CaptureLightGizmo::Space::World};
  CaptureLightGizmo::Axis axis{CaptureLightGizmo::Axis::Free};
  glm::vec2 delta{};
  bool snap{};
};
struct CaptureEvent {
  uint32_t frame{0};
  bool reset{false}, skip{false};
  bool acquireOutOfDate{false}, presentSuboptimal{false};
  bool guiUnload{false};
  bool guiWaitForLoad{true};
  std::optional<bool> taa, orthographic, objectVisible, minimized;
  std::optional<int> animationClip;
  std::optional<uint32_t> gltfScene;
  std::optional<double> animationTime;
  std::optional<bool> animationPlaying;
  std::optional<bool> rayShadowDenoise;
  std::optional<bool> ddgiPaused, ddgiEnabled;
  std::optional<uint32_t> ddgiFitProbes;
  std::optional<uint32_t> rayShadowSamples;
  std::optional<uint32_t> areaLightSamples;
  std::optional<uint32_t> samples;
  std::optional<uint32_t> bimHiddenObject;
  std::optional<bool> bimRoomVolumesVisible;
  std::optional<uint32_t> bimSelectedObject;
  std::optional<int> bimLodBias;
  std::optional<glm::vec4> sectionPlane;
  std::optional<glm::vec3> areaLightPosition;
  std::optional<float> exposure;
  std::optional<glm::uvec2> resize;
  std::optional<glm::uvec2> depthProbe;
  std::string technique{}, reload{}, guiReload{}, guiSampleModel{};
  std::string rayShadows{};
  std::string areaLighting{};
  std::optional<CaptureLightEdit> lightEdit;
  std::optional<CaptureLightTarget> lightSelection;
  std::optional<CaptureLightAdd> lightAdd;
  std::optional<CaptureNodeEdit> nodeEdit;
  std::string addPrimitive;
  std::string sceneHistory;
  bool expectLightEditFailure{};
};
struct CaptureSample {
  std::optional<CaptureCamera> camera;
  std::optional<glm::vec3> objectTranslation, bimTranslation;
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
    readTranslations(json, "bimTranslation", bim_);
    for (const auto &value : json.value("events", nlohmann::json::array())) {
      CaptureEvent event;
      event.frame = value.at("frame").get<uint32_t>();
      checkFrame(event.frame);
      event.reset = value.value("reset", false);
      event.skip = value.value("skip", false);
      if (value.contains("lightEdit")) event.lightEdit = lightEditRequest(value.at("lightEdit"));
      if (value.contains("lightSelection")) event.lightSelection = lightTarget(value.at("lightSelection"));
      if (value.contains("lightAdd")) event.lightAdd = lightAddRequest(value.at("lightAdd"));
      if (value.contains("nodeEdit")) event.nodeEdit = nodeEditRequest(value.at("nodeEdit"));
      if (value.contains("addPrimitive")) {
        const auto& primitive = value.at("addPrimitive");
        if (!primitive.is_string() || primitive.get_ref<const std::string&>().empty())
          throw std::invalid_argument("Capture addPrimitive must name a primitive");
        event.addPrimitive = primitive.get<std::string>();
      }
      if (value.contains("sceneHistory")) {
        const auto& history = value.at("sceneHistory");
        if (history != "undo" && history != "redo")
          throw std::invalid_argument("Capture sceneHistory must be undo or redo");
        event.sceneHistory = history.get<std::string>();
      }
      if (value.contains("expectLightEditFailure")) {
        const auto& expected = value.at("expectLightEditFailure");
        if (!expected.is_boolean()) throw std::invalid_argument("Capture light failure expectation must be boolean");
        event.expectLightEditFailure = expected.get<bool>();
      }
      const unsigned lightActions = unsigned(event.lightEdit.has_value()) + unsigned(event.lightSelection.has_value()) + unsigned(event.lightAdd.has_value());
      const unsigned sceneActions = unsigned(event.nodeEdit.has_value()) +
          unsigned(!event.sceneHistory.empty()) + unsigned(!event.addPrimitive.empty());
      if (lightActions + sceneActions > 1 || (value.contains("expectLightEditFailure") && !lightActions))
        throw std::invalid_argument("Use one edit or history action per capture frame");
      if (lightActions || sceneActions)
        nodeFields(value, {"frame", "gltfScene", "lightEdit", "lightSelection", "lightAdd", "nodeEdit", "addPrimitive", "sceneHistory", "expectLightEditFailure",
                           "taa", "exposure", "resize", "depthProbe", "technique", "rayShadows", "rayShadowSamples",
                           "rayShadowDenoise", "areaLighting", "areaLightSamples", "ddgiPaused", "ddgiEnabled", "samples", "reset"},
                   "edit event");
      event.acquireOutOfDate = value.value("acquireOutOfDate", false);
      event.presentSuboptimal = value.value("presentSuboptimal", false);
      if (value.contains("bimHiddenObject"))
        event.bimHiddenObject = value.at("bimHiddenObject").get<uint32_t>();
      if (value.contains("bimRoomVolumesVisible"))
        event.bimRoomVolumesVisible = value.at("bimRoomVolumesVisible").get<bool>();
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
      boolean("orthographic", event.orthographic);
      boolean("objectVisible", event.objectVisible);
      boolean("minimized", event.minimized);
      for (const char *name : {"ddgiPaused", "ddgiEnabled"}) {
        if (value.contains(name) && !value.at(name).is_boolean())
          throw std::invalid_argument("Capture DDGI flags must be booleans");
      }
      boolean("ddgiPaused", event.ddgiPaused);
      boolean("ddgiEnabled", event.ddgiEnabled);
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
      if (value.contains("gltfScene")) {
        const auto& scene = value.at("gltfScene");
        if (!scene.is_number_integer() || scene < 0 || scene > std::numeric_limits<int>::max())
          throw std::invalid_argument("Capture gltfScene must be a nonnegative scene index");
        event.gltfScene = scene.get<uint32_t>();
      }
      if (value.contains("animationClip")) {
        const auto& index = value.at("animationClip");
        if (!index.is_number_integer() || index.get<double>() < -1 ||
            index.get<double>() > std::numeric_limits<int>::max())
          throw std::invalid_argument("Capture animationClip must be -1 or a nonnegative clip index");
        event.animationClip = index.get<int>();
      }
      if (value.contains("animationTime")) {
        const auto& time = value.at("animationTime");
        if (!time.is_number() || !std::isfinite(time.get<double>()) || time.get<double>() < 0)
          throw std::invalid_argument("Capture animationTime must be finite and nonnegative");
        event.animationTime = time.get<double>();
      }
      if (value.contains("animationPlaying")) {
        if (!value.at("animationPlaying").is_boolean())
          throw std::invalid_argument("Capture animationPlaying must be a boolean");
        event.animationPlaying = value.at("animationPlaying").get<bool>();
      }
      event.reload = value.value("reload", std::string{});
      event.guiReload = value.value("guiReload", std::string{});
      event.guiSampleModel = value.value("guiSampleModel", std::string{});
      if (value.contains("guiUnload") && !value.at("guiUnload").is_boolean())
        throw std::invalid_argument("Capture guiUnload must be a boolean");
      event.guiUnload = value.value("guiUnload", false);
      if (unsigned(!event.reload.empty()) + unsigned(!event.guiReload.empty()) +
              unsigned(!event.guiSampleModel.empty()) + unsigned(event.guiUnload) >
          1u)
        throw std::invalid_argument("Choose one model reload route per event");
      if (value.contains("guiWaitForLoad") &&
          !value.at("guiWaitForLoad").is_boolean())
        throw std::invalid_argument("Capture guiWaitForLoad must be a boolean");
      event.guiWaitForLoad = value.value("guiWaitForLoad", true);
      if (!event.guiWaitForLoad && event.guiReload.empty() &&
          event.guiSampleModel.empty() && !event.guiUnload)
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
  uint32_t frameCount{0};
  std::vector<uint32_t> sampleFrames;

private:
  template <class T> struct Key {
    uint32_t frame;
    T value;
  };
  std::vector<Key<CaptureCamera>> cameras_;
  std::vector<Key<glm::vec3>> objects_, bim_;
  std::vector<CaptureEvent> events_;
  uint32_t objectNode_{0};
  std::string objectName_{};
  void checkFrame(uint32_t frame) const {
    if (frame == 0 || frame > frameCount)
      throw std::invalid_argument("Temporal capture frame outside sequence");
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
      if(value.contains("sourceNode")) result.sourceNode=int32_t(lightInteger(value.at("sourceNode"),0,INT32_MAX));
      else if(value.contains("nodeName")) {
        const auto& name=value.at("nodeName");if(!name.is_string() || name.get_ref<const std::string&>().size()>4096)
          throw std::invalid_argument("Capture light nodeName must be a bounded string");
        result.nodeName=name.get<std::string>();
        if (result.nodeName.empty() || result.nodeName.find('\0') != std::string::npos)
          throw std::invalid_argument("Capture light nodeName must name a node");
      } else throw std::invalid_argument("Imported capture lights require sourceNode or nodeName");
    } else {
      if(!value.contains("index")) throw std::invalid_argument("Manual and generated capture lights require index");
      result.index=uint32_t(lightInteger(value.at("index"),0,UINT32_MAX-1));
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
    else {if(!value.contains("gesture")) throw std::invalid_argument("Capture light gesture phase requires gesture identity");result.gesture=lightInteger(value.at("gesture"),1,9007199254740991ull);}
    if(value.contains("expectedLightRevision")) result.expectedLightRevision=lightInteger(value.at("expectedLightRevision"),1,9007199254740991ull);
    const auto payloads=unsigned(value.contains("values"))+unsigned(value.contains("gizmo"));
    if((result.phase==Phase::End || result.phase==Phase::Cancel)?payloads!=0:payloads!=1)
      throw std::invalid_argument("Capture light commit/begin/update require exactly one payload; end/cancel carry none");
    if(value.contains("values")) result.values=lightValues(value.at("values"),result.target.type);
    if(value.contains("gizmo")) result.gizmo=lightGizmo(value.at("gizmo"));return result;
  }
  static CaptureLightAdd lightAddRequest(const nlohmann::json& value) {
    nodeFields(value,{"type","expectedLightRevision"},"light add");if(!value.contains("type")) throw std::invalid_argument("Capture light add requires type");
    CaptureLightAdd result;result.type=lightType(value.at("type"));
    if(value.contains("expectedLightRevision")) result.expectedLightRevision=lightInteger(value.at("expectedLightRevision"),1,9007199254740991ull);return result;
  }
  static uint64_t lightInteger(const nlohmann::json& value,uint64_t minimum,uint64_t maximum) {
    if(!value.is_number_integer() || (value.is_number_unsigned()?value.get<uint64_t>()>maximum:
        value.get<int64_t>()<0 || uint64_t(value.get<int64_t>())>maximum) || value.get<uint64_t>()<minimum)
      throw std::invalid_argument("Capture light identity must be an integer within its declared bounds");
    return value.get<uint64_t>();
  }
  static uint32_t captureNodeIndex(const nlohmann::json& value) {
    if (!value.is_number_integer() || value.get<double>() < 0 || value.get<double>() > INT32_MAX)
      throw std::invalid_argument("Capture node index must be a nonnegative int32");
    return value.get<uint32_t>();
  }

  static CaptureNodeEdit nodeEditRequest(const nlohmann::json& value) {
    using Operation = CaptureNodeEdit::Operation;
    nodeFields(value, {"sourceNode", "graphNode", "operation", "matrix", "visible", "parent", "gizmo", "delta"}, "node edit");
    CaptureNodeEdit edit;
    if (value.contains("sourceNode") == value.contains("graphNode"))
      throw std::invalid_argument("Capture node edit needs exactly one node target");
    if (value.contains("graphNode")) edit.graphNode = captureNodeIndex(value.at("graphNode"));
    else edit.sourceNode = captureNodeIndex(value.at("sourceNode"));
    const auto op = value.at("operation").get<std::string>();
    if (op == "select" || op == "end" || op == "cancel") {
      if (value.size() != 2) throw std::invalid_argument("Capture node action has unexpected fields");
      edit.operation = op == "select" ? Operation::Select : op == "end" ? Operation::End : Operation::Cancel;
    } else if (op == "transform") {
      const auto& matrix = value.at("matrix");
      if (value.size() != 3 || !matrix.is_array() || matrix.size() != 16)
        throw std::invalid_argument("Capture node transform needs a column-major matrix");
      for (size_t i = 0; i < 16; ++i) edit.matrix[i / 4][i % 4] = lightNumber(matrix[i]);
      edit.operation = Operation::Transform;
    } else if (op == "visibility") {
      if (value.size() != 3 || !value.at("visible").is_boolean())
        throw std::invalid_argument("Capture node visibility needs a boolean");
      edit.visible = value.at("visible").get<bool>();
      edit.operation = Operation::Visibility;
    } else if (op == "reparent") {
      if (value.size() != 3) throw std::invalid_argument("Capture reparent needs exactly one parent");
      const auto& parent = value.at("parent");
      if (!parent.is_null()) edit.parent = captureNodeIndex(parent);
      edit.operation = Operation::Reparent;
    } else if (op == "begin" || op == "update") {
      edit.delta = lightVector<2>(value.at("delta"));
      if (glm::any(glm::greaterThan(glm::abs(edit.delta), glm::vec2(16384))))
        throw std::invalid_argument("Capture node drag exceeds its screen delta limit");
      if (op == "begin") {
        if (value.size() != 4) throw std::invalid_argument("Capture node drag begin needs a gizmo and delta");
        auto settings = value.at("gizmo");
        nodeFields(settings, {"tool", "space", "axis", "snap"}, "node gizmo");
        settings["amount"] = {0, 0, 0};
        const auto gizmo = lightGizmo(settings);
        edit.tool = gizmo.tool;
        edit.space = gizmo.space;
        edit.axis = gizmo.axis;
        edit.snap = gizmo.snap;
        edit.operation = Operation::Begin;
      } else {
        if (value.size() != 3) throw std::invalid_argument("Capture node drag update needs only a delta");
        edit.operation = Operation::Update;
      }
    } else {
      throw std::invalid_argument("Unknown capture node operation");
    }
    return edit;
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
