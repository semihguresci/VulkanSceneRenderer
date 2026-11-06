#include "Container/geometry/GltfSky.h"

#include <tiny_gltf.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string_view>

namespace container::geometry::gltf {
namespace {

constexpr char kExtension[] = "OMI_environment_sky";

[[noreturn]] void invalid(std::string_view reason) {
  throw std::invalid_argument(std::string(kExtension) + ": " + std::string(reason));
}

double number(const tinygltf::Value& value, double low, double high) {
  if (!value.IsNumber())
    invalid("expected a numeric value");
  const double n = value.GetNumberAsDouble();
  if (!std::isfinite(n) || n < low || n > high)
    invalid("numeric value outside the supported range");
  return n;
}

std::array<float, 3> color(const tinygltf::Value& parent, const char* key) {
  if (!parent.Has(key))
    invalid("missing sky color");
  const auto& value = parent.Get(key);
  if (!value.IsArray() || value.ArrayLen() != 3)
    invalid("sky colors must have three channels");
  return {float(number(value.Get(0), 0, 1)), float(number(value.Get(1), 0, 1)),
          float(number(value.Get(2), 0, 1))};
}

std::array<float, 3> rotatedUp(const tinygltf::Value& rotation) {
  if (!rotation.IsArray() || rotation.ArrayLen() != 4)
    invalid("rotation must be an x/y/z/w quaternion");
  const double x = number(rotation.Get(0), -1, 1);
  const double y = number(rotation.Get(1), -1, 1);
  const double z = number(rotation.Get(2), -1, 1);
  const double w = number(rotation.Get(3), -1, 1);
  const double lengthSquared = x*x + y*y + z*z + w*w;
  if (std::abs(lengthSquared - 1.0) > 0.001)
    invalid("rotation quaternion must have unit length");
  const double scale = 2.0 / lengthSquared;
  return {float(scale * (x*y - w*z)), float(1 - scale * (x*x + z*z)),
          float(scale * (y*z + w*x))};
}

} // namespace

std::optional<renderer::SkySettings>
readAuthoredSky(const tinygltf::Model& model, size_t sceneIndex) {
  const auto root = model.extensions.find(kExtension);
  if (root == model.extensions.end())
    return std::nullopt;
  if (std::ranges::find(model.extensionsUsed, kExtension) == model.extensionsUsed.end())
    invalid("extension is missing from extensionsUsed");
  if (sceneIndex >= model.scenes.size() || !root->second.IsObject() || !root->second.Has("skies"))
    invalid("missing scene or skies array");
  const auto& skies = root->second.Get("skies");
  if (!skies.IsArray() || skies.ArrayLen() == 0)
    invalid("skies must be a nonempty array");
  int skyIndex = 0;
  const auto& sceneExtensions = model.scenes[sceneIndex].extensions;
  if (const auto ref = sceneExtensions.find(kExtension); ref != sceneExtensions.end()) {
    if (!ref->second.IsObject() || !ref->second.Has("sky") || !ref->second.Get("sky").IsInt())
      invalid("scene sky reference must be an integer");
    skyIndex = ref->second.Get("sky").Get<int>();
  }
  if (skyIndex < 0 || static_cast<size_t>(skyIndex) >= skies.ArrayLen())
    invalid("scene sky reference is out of range");
  const auto& sky = skies.Get(skyIndex);
  if (!sky.IsObject() || !sky.Has("type") || !sky.Get("type").IsString())
    invalid("sky type is missing");
  const auto& type = sky.Get("type").Get<std::string>();
  if (type != "plain" && type != "gradient")
    invalid("only plain and gradient skies are supported");
  if (!sky.Has(type) || !sky.Get(type).IsObject())
    invalid("missing sky parameters");

  renderer::SkySettings result;
  if (sky.Has("rotation"))
    result.gradientUp = rotatedUp(sky.Get("rotation"));
  const auto& parameters = sky.Get(type);
  if (type == "plain") {
    result.mode = renderer::SkyMode::Plain;
    result.horizon = color(parameters, "color");
  } else {
    result.mode = renderer::SkyMode::Gradient;
    result.horizon = color(parameters, "horizonColor");
    result.zenith = color(parameters, "topColor");
    result.ground = color(parameters, "bottomColor");
    result.zenithCurve = parameters.Has("topCurve")
        ? float(number(parameters.Get("topCurve"), 0.001, 1)) : 0.15f;
    result.groundCurve = parameters.Has("bottomCurve")
        ? float(number(parameters.Get("bottomCurve"), 0.001, 1)) : 0.02f;
  }
  return result;
}

} // namespace container::geometry::gltf
