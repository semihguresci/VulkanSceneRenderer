#pragma once

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace container::temporal {

struct CaptureCamera {
  glm::vec3 position{}, target{};
};
struct CaptureEvent {
  uint32_t frame{0};
  bool reset{false}, skip{false};
  bool acquireOutOfDate{false}, presentSuboptimal{false};
  std::optional<bool> taa, orthographic, objectVisible, minimized;
  std::optional<uint32_t> samples;
  std::optional<uint32_t> bimHiddenObject;
  std::optional<int> bimLodBias;
  std::optional<glm::vec4> sectionPlane;
  std::optional<float> exposure;
  std::optional<glm::uvec2> resize;
  std::string technique{}, reload{};
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
      event.acquireOutOfDate = value.value("acquireOutOfDate", false);
      event.presentSuboptimal = value.value("presentSuboptimal", false);
      if (value.contains("bimHiddenObject"))
        event.bimHiddenObject = value.at("bimHiddenObject").get<uint32_t>();
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
      auto boolean = [&](const char *name, std::optional<bool> &target) {
        if (value.contains(name))
          target = value.at(name).get<bool>();
      };
      boolean("taa", event.taa);
      boolean("orthographic", event.orthographic);
      boolean("objectVisible", event.objectVisible);
      boolean("minimized", event.minimized);
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
      event.technique = value.value("technique", std::string{});
      if (!event.technique.empty() && event.technique != "forward-raster" &&
          event.technique != "deferred-raster")
        throw std::invalid_argument("Invalid capture render technique");
      event.reload = value.value("reload", std::string{});
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
            return CaptureCamera{glm::mix(a.position, b.position, t),
                                 glm::mix(a.target, b.target, t)};
          });
    }
    if (!objects_.empty())
      result.objectTranslation =
          interpolate(objects_, frame, [](auto a, auto b, float t) {
            return glm::mix(a, b, t);
          });
    if (!bim_.empty())
      result.bimTranslation =
          interpolate(bim_, frame, [](auto a, auto b, float t) {
            return glm::mix(a, b, t);
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
  template <class T, class Mix>
  static T interpolate(const std::vector<Key<T>> &keys, uint32_t frame,
                       Mix mix) {
    if (frame <= keys.front().frame)
      return keys.front().value;
    for (size_t i = 1; i < keys.size(); ++i)
      if (frame <= keys[i].frame) {
        const float t = float(frame - keys[i - 1].frame) /
                        float(keys[i].frame - keys[i - 1].frame);
        return mix(keys[i - 1].value, keys[i].value, t);
      }
    return keys.back().value;
  }
};
} // namespace container::temporal
