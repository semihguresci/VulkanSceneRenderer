#include <Container/geometry/GltfAnimation.h>
#include <Container/geometry/GltfAccessor.h>

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>

namespace container::geometry::gltf {
namespace {
[[noreturn]] void invalid(const std::string& message) {
  throw std::invalid_argument(message);
}
size_t product(size_t a, size_t b) {
  if (b && a > std::numeric_limits<size_t>::max() / b)
    invalid("animation accessor size overflow");
  return a * b;
}
template<class T> const T& checked(const std::vector<T>& values, int index,
                                  const char* description) {
  if (index < 0 || static_cast<size_t>(index) >= values.size())
    invalid(std::string("invalid animation ") + description + " index");
  return values[static_cast<size_t>(index)];
}
size_t vectorComponents(int type) {
  switch (type) {
    case TINYGLTF_TYPE_SCALAR: return 1;
    case TINYGLTF_TYPE_VEC2: return 2;
    case TINYGLTF_TYPE_VEC3: return 3;
    case TINYGLTF_TYPE_VEC4: return 4;
    default: invalid("animation accessor must be scalar or vector");
  }
}
std::vector<double> readAccessor(const tinygltf::Model& model, int index, size_t& remainingBytes) {
  try {
    CheckedAccessor reader(model,index,"animation accessor",{AccessorUsage::Generic,
      HasRequiredExtension(model,"KHR_accessor_float64")});
    (void)vectorComponents(reader.accessor().type);
    const size_t count = product(reader.count(),reader.components());
    const size_t bytes = product(count,sizeof(double));
    // Bound the entire library, including repeated sampler/channel expansions;
    // sparse zero bases must not bypass an encoded input-memory budget.
    if (count > 16*1024*1024 || bytes > remainingBytes) invalid("animation decoded accessors exceed safety limit");
    remainingBytes -= bytes;
    std::vector<double> values(count);
    for (size_t i = 0; i < reader.count(); ++i)
      reader.readDoubles(i,std::span(values).subspan(i*reader.components(),reader.components()));
    return values;
  } catch (const std::runtime_error& error) { invalid(error.what()); }
}
std::vector<std::string> splitPointer(const std::string& pointer) {
  if (pointer.empty() || pointer[0] != '/') invalid("invalid animation JSON pointer");
  std::vector<std::string> parts;
  size_t begin = 1;
  while (true) {
    const size_t end = pointer.find('/', begin);
    std::string token = pointer.substr(begin, end - begin), decoded;
    for (size_t i = 0; i < token.size(); ++i) {
      if (token[i] != '~') decoded += token[i];
      else {
        if (++i == token.size() || (token[i] != '0' && token[i] != '1'))
          invalid("invalid animation JSON pointer escape");
        decoded += token[i] == '0' ? '~' : '/';
      }
    }
    parts.push_back(std::move(decoded));
    if (end == std::string::npos) break;
    begin = end + 1;
  }
  return parts;
}
std::optional<size_t> indexToken(const std::string& value) {
  size_t result{};
  if (value.empty() || (value.size() > 1 && value[0] == '0')) return {};
  const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
  if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) return {};
  return result;
}
size_t requiredIndex(const std::string& value, size_t count) {
  const auto index = indexToken(value);
  if (!index || *index >= count) invalid("animation pointer index out of range");
  return *index;
}
std::string join(const std::vector<std::string>& parts, size_t begin, size_t end) {
  std::string result;
  for (size_t i = begin; i < end; ++i) {
    if (i != begin) result += '/';
    result += parts[i];
  }
  return result;
}
size_t weightCount(const tinygltf::Model& model, size_t nodeIndex) {
  const auto& node = model.nodes[nodeIndex];
  const auto& mesh = checked(model.meshes, node.mesh, "morph mesh");
  if (mesh.primitives.empty() || mesh.primitives[0].targets.empty())
    invalid("animation weights target has no morph targets");
  const size_t count = mesh.primitives[0].targets.size();
  for (const auto& primitive : mesh.primitives)
    if (primitive.targets.size() != count) invalid("inconsistent morph target counts");
  if ((!node.weights.empty() && node.weights.size() != count) ||
      (!mesh.weights.empty() && mesh.weights.size() != count))
    invalid("morph default weights count mismatch");
  return count;
}
AnimationTarget nodeTarget(const tinygltf::Model& model, size_t index,
                           const std::string& property) {
  if (index >= model.nodes.size()) invalid("animation node index out of range");
  AnimationTarget target{AnimationTargetKind::NodeTranslation, index, property, 3, {}};
  if (property == "rotation") { target.kind = AnimationTargetKind::NodeRotation; target.components = 4; }
  else if (property == "scale") target.kind = AnimationTargetKind::NodeScale;
  else if (property == "weights") {
    target.kind = AnimationTargetKind::NodeWeights;
    target.components = weightCount(model, index);
  } else if (property != "translation") invalid("unsupported animation node property");
  if (property != "weights" && !model.nodes[index].matrix.empty())
    invalid("TRS animation cannot target a node with a matrix");
  return target;
}
// A deliberate supported subset of mutable material properties. Unknown
// properties are surfaced as diagnostics instead of advertising full pointer
// support. Extension objects must exist; defaults of their numeric leaves may
// be animated without an explicit leaf in the source JSON.
const std::map<std::string, size_t>& materialProperties() {
  static const auto properties = [] {
    std::map<std::string, size_t> result{
    {"pbrMetallicRoughness/baseColorFactor",4}, {"pbrMetallicRoughness/metallicFactor",1},
    {"pbrMetallicRoughness/roughnessFactor",1}, {"emissiveFactor",3}, {"alphaCutoff",1},
    {"normalTexture/scale",1}, {"occlusionTexture/strength",1},
    {"extensions/KHR_materials_emissive_strength/emissiveStrength",1},
    {"extensions/KHR_materials_ior/ior",1},
    {"extensions/KHR_materials_dispersion/dispersion",1},
    {"extensions/KHR_materials_transmission/transmissionFactor",1},
    {"extensions/KHR_materials_volume/thicknessFactor",1},
    {"extensions/KHR_materials_volume/attenuationDistance",1},
    {"extensions/KHR_materials_volume/attenuationColor",3},
    {"extensions/KHR_materials_clearcoat/clearcoatFactor",1},
    {"extensions/KHR_materials_clearcoat/clearcoatRoughnessFactor",1},
    {"extensions/KHR_materials_clearcoat/clearcoatNormalTexture/scale",1},
    {"extensions/KHR_materials_sheen/sheenColorFactor",3},
    {"extensions/KHR_materials_sheen/sheenRoughnessFactor",1},
    {"extensions/KHR_materials_specular/specularFactor",1},
    {"extensions/KHR_materials_specular/specularColorFactor",3},
    {"extensions/KHR_materials_iridescence/iridescenceFactor",1},
    {"extensions/KHR_materials_iridescence/iridescenceIor",1},
    {"extensions/KHR_materials_iridescence/iridescenceThicknessMinimum",1},
    {"extensions/KHR_materials_iridescence/iridescenceThicknessMaximum",1},
    {"extensions/KHR_materials_anisotropy/anisotropyStrength",1},
    {"extensions/KHR_materials_anisotropy/anisotropyRotation",1},
    {"extensions/KHR_materials_diffuse_transmission/diffuseTransmissionFactor",1},
    {"extensions/KHR_materials_diffuse_transmission/diffuseTransmissionColorFactor",3},
    {"extensions/KHR_materials_pbrSpecularGlossiness/diffuseFactor",4},
    {"extensions/KHR_materials_pbrSpecularGlossiness/specularFactor",3},
    {"extensions/KHR_materials_pbrSpecularGlossiness/glossinessFactor",1}
    };
    for (const char* texture : {
      "pbrMetallicRoughness/baseColorTexture", "pbrMetallicRoughness/metallicRoughnessTexture",
      "normalTexture", "occlusionTexture", "emissiveTexture",
      "extensions/KHR_materials_specular/specularTexture",
      "extensions/KHR_materials_specular/specularColorTexture",
      "extensions/KHR_materials_transmission/transmissionTexture",
      "extensions/KHR_materials_clearcoat/clearcoatTexture",
      "extensions/KHR_materials_clearcoat/clearcoatRoughnessTexture",
      "extensions/KHR_materials_clearcoat/clearcoatNormalTexture",
      "extensions/KHR_materials_volume/thicknessTexture",
      "extensions/KHR_materials_sheen/sheenColorTexture",
      "extensions/KHR_materials_sheen/sheenRoughnessTexture",
      "extensions/KHR_materials_iridescence/iridescenceTexture",
      "extensions/KHR_materials_iridescence/iridescenceThicknessTexture",
      "extensions/KHR_materials_anisotropy/anisotropyTexture",
      "extensions/KHR_materials_diffuse_transmission/diffuseTransmissionTexture",
      "extensions/KHR_materials_diffuse_transmission/diffuseTransmissionColorTexture",
      "extensions/KHR_materials_pbrSpecularGlossiness/diffuseTexture",
      "extensions/KHR_materials_pbrSpecularGlossiness/specularGlossinessTexture"}) {
      const auto prefix = std::string(texture) + "/extensions/KHR_texture_transform/";
      result.emplace(prefix + "offset", 2);
      result.emplace(prefix + "scale", 2);
      result.emplace(prefix + "rotation", 1);
    }
    return result;
  }();
  return properties;
}
bool extendedUnitIntervalProperty(std::string_view property) {
  return property == "extensions/KHR_materials_anisotropy/anisotropyStrength" ||
      property == "extensions/KHR_materials_diffuse_transmission/diffuseTransmissionFactor" ||
      property == "extensions/KHR_materials_diffuse_transmission/diffuseTransmissionColorFactor";
}
template<class Material>
auto* coreTextureExtensions(Material& material, const std::string& path) {
  // All three tinygltf texture-info types retain the same ExtensionMap. Never
  // serialize/recreate the texture info: optional texCoord/extras must survive.
  using Map = std::conditional_t<std::is_const_v<Material>, const tinygltf::ExtensionMap, tinygltf::ExtensionMap>;
  Map* extensions = nullptr;
  int index = -1;
  if (path == "pbrMetallicRoughness/baseColorTexture") {
    extensions = &material.pbrMetallicRoughness.baseColorTexture.extensions;
    index = material.pbrMetallicRoughness.baseColorTexture.index;
  } else if (path == "pbrMetallicRoughness/metallicRoughnessTexture") {
    extensions = &material.pbrMetallicRoughness.metallicRoughnessTexture.extensions;
    index = material.pbrMetallicRoughness.metallicRoughnessTexture.index;
  } else if (path == "normalTexture") { extensions = &material.normalTexture.extensions; index = material.normalTexture.index; }
  else if (path == "occlusionTexture") { extensions = &material.occlusionTexture.extensions; index = material.occlusionTexture.index; }
  else if (path == "emissiveTexture") { extensions = &material.emissiveTexture.extensions; index = material.emissiveTexture.index; }
  if (!extensions || index < 0) invalid("animation pointer targets absent material texture object");
  return extensions;
}
template<class Material>
auto* materialPropertyParent(Material& material, const std::vector<std::string>& tokens) {
  using Value = std::conditional_t<std::is_const_v<Material>, const tinygltf::Value, tinygltf::Value>;
  Value* parent = nullptr;
  size_t next = 2;
  if (tokens[0] == "extensions") {
    const auto found = material.extensions.find(tokens[1]);
    if (found != material.extensions.end()) parent = &found->second;
  } else {
    const auto transform = std::find(tokens.begin(), tokens.end(), "extensions");
    const size_t begin = static_cast<size_t>(transform - tokens.begin());
    auto* extensions = coreTextureExtensions(material, join(tokens,0,begin));
    const auto found = extensions->find("KHR_texture_transform");
    if (found != extensions->end()) parent = &found->second;
    next = begin + 2;
  }
  if (!parent || !parent->IsObject()) invalid("animation pointer targets absent material parent object");
  for (size_t i = next; i + 1 < tokens.size(); ++i) {
    auto& object = parent->template Get<tinygltf::Value::Object>();
    const auto found = object.find(tokens[i]);
    if (found == object.end() || !found->second.IsObject())
      invalid("animation pointer targets absent material parent object");
    parent = &found->second;
    // Extension texture infos must exist, including their required index.
    if (i == 2 && tokens[0] == "extensions" && tokens[i].ends_with("Texture")) {
      if (!parent->Has("index") || !parent->Get("index").IsInt() || parent->Get("index").template Get<int>() < 0)
        invalid("animation pointer targets absent material texture object");
    }
  }
  return parent;
}
void validateMaterialLeaf(const tinygltf::Value& parent, const std::string& leaf, size_t components,
                          bool unitInterval) {
  if (!parent.Has(leaf)) return; // A schema default is defined by its existing parent.
  const auto& value = parent.Get(leaf);
  if (components == 1) {
    if (!value.IsNumber() || !std::isfinite(value.GetNumberAsDouble())) invalid("invalid material animation scalar property");
    if (unitInterval && (value.GetNumberAsDouble() < 0 || value.GetNumberAsDouble() > 1))
      invalid("material animation scalar property is outside [0,1]");
  } else {
    if (!value.IsArray() || value.ArrayLen() != components) invalid("invalid material animation vector property");
    for (size_t i = 0; i < components; ++i)
      if (!value.Get(i).IsNumber() || !std::isfinite(value.Get(i).GetNumberAsDouble()))
        invalid("invalid material animation vector component");
      else if (unitInterval && (value.Get(i).GetNumberAsDouble() < 0 || value.Get(i).GetNumberAsDouble() > 1))
        invalid("material animation vector component is outside [0,1]");
  }
}
std::optional<AnimationTarget> pointerTarget(const tinygltf::Model& model,
                                            const std::string& pointer) {
  auto parts = splitPointer(pointer);
  // All supported schema tokens have literal names without '/' or '~'. Keep
  // RFC 6901 escapes from being mistaken for hierarchy separators when forming
  // the canonical relative property below.
  for (const auto& part : parts)
    if (part.find('/') != std::string::npos || part.find('~') != std::string::npos) return {};
  AnimationTarget target;
  size_t propertyBegin = 2;
  if (parts.size() >= 3 && parts[0] == "nodes") {
    const size_t index = requiredIndex(parts[1], model.nodes.size());
    if (parts[2] != "translation" && parts[2] != "rotation" &&
        parts[2] != "scale" && parts[2] != "weights") return {};
    target = nodeTarget(model, index, parts[2]);
    if (parts.size() == 4) {
      // Quaternion components are not independent mutable properties.
      if (target.kind == AnimationTargetKind::NodeRotation) return {};
      target.element = requiredIndex(parts[3], target.components);
    } else if (parts.size() != 3) return {};
    return target;
  }
  if (parts.size() >= 3 && parts[0] == "materials") {
    target.kind = AnimationTargetKind::Material;
    target.index = requiredIndex(parts[1], model.materials.size());
  } else if (parts.size() >= 3 && parts[0] == "cameras") {
    target.kind = AnimationTargetKind::Camera;
    target.index = requiredIndex(parts[1], model.cameras.size());
  } else if (parts.size() >= 5 && parts[0] == "extensions" &&
             parts[1] == "KHR_lights_punctual" && parts[2] == "lights") {
    target.kind = AnimationTargetKind::Light;
    target.index = requiredIndex(parts[3], model.lights.size());
    propertyBegin = 4;
  } else return {};
  if (parts.size() > propertyBegin + 1 && indexToken(parts.back())) {
    target.element = *indexToken(parts.back());
    parts.pop_back();
  }
  target.property = join(parts, propertyBegin, parts.size());
  if (target.kind == AnimationTargetKind::Material) {
    const auto found = materialProperties().find(target.property);
    if (found == materialProperties().end()) return {};
    target.components = found->second;
    const auto& material = model.materials[target.index];
    if (parts[propertyBegin] == "extensions" || target.property.find("/extensions/KHR_texture_transform/") != std::string::npos) {
      const auto tokens = splitPointer('/' + target.property);
      validateMaterialLeaf(*materialPropertyParent(material,tokens),tokens.back(),target.components,
          extendedUnitIntervalProperty(target.property));
    }
    if ((target.property == "normalTexture/scale" && material.normalTexture.index < 0) ||
        (target.property == "occlusionTexture/strength" && material.occlusionTexture.index < 0))
      invalid("animation pointer targets absent material texture object");
  } else if (target.kind == AnimationTargetKind::Light) {
    if (target.property == "color") target.components = 3;
    else if (target.property == "intensity" || target.property == "range" ||
             target.property == "spot/innerConeAngle" || target.property == "spot/outerConeAngle")
      target.components = 1;
    else return {};
    const auto& light = model.lights[target.index];
    if (target.property.starts_with("spot/") && light.type != "spot")
      invalid("animation pointer targets absent light spot object");
    if (target.property == "range" && light.range == 0.0)
      invalid("animation pointer targets undefined light range");
  } else {
    const auto& camera = model.cameras[target.index];
    if (camera.type == "perspective" &&
        (target.property == "perspective/yfov" || target.property == "perspective/znear" ||
         target.property == "perspective/zfar" || target.property == "perspective/aspectRatio")) {
      if ((target.property == "perspective/zfar" && camera.perspective.zfar == 0.0) ||
          (target.property == "perspective/aspectRatio" && camera.perspective.aspectRatio == 0.0))
        invalid("animation pointer targets undefined optional camera property");
      target.components = 1;
    } else if (camera.type == "orthographic" &&
        (target.property == "orthographic/xmag" || target.property == "orthographic/ymag" ||
         target.property == "orthographic/znear" || target.property == "orthographic/zfar"))
      target.components = 1;
    else return {};
  }
  if (target.element && (target.components == 1 || *target.element >= target.components))
    invalid("animation pointer vector component out of range");
  return target;
}
size_t sampleComponents(const AnimationTarget& target) {
  return target.element ? 1 : target.components;
}
bool overlapping(const AnimationTarget& a, const AnimationTarget& b) {
  return a.kind == b.kind && a.index == b.index && a.property == b.property &&
         (!a.element || !b.element || a.element == b.element);
}
void normalizeQuaternion(std::vector<double>& value) {
  const double norm = std::hypot(std::hypot(value[0], value[1]), std::hypot(value[2], value[3]));
  if (!std::isfinite(norm) || norm < 1e-15) invalid("animation produced a zero/non-finite quaternion");
  for (double& component : value) component /= norm;
}
std::vector<double> sampleTrack(const AnimationTrack& track, double time) {
  const size_t width = sampleComponents(track.target);
  const bool cubic = track.interpolation == AnimationInterpolation::CubicSpline;
  const size_t stride = width * (cubic ? 3 : 1);
  const auto keyValue = [&](size_t key) {
    const auto first = track.values.begin() + key * stride + (cubic ? width : 0);
    return std::vector<double>(first, first + width);
  };
  const auto upper = std::upper_bound(track.times.begin(), track.times.end(), time);
  size_t left = upper == track.times.begin() ? 0 : static_cast<size_t>(upper - track.times.begin() - 1);
  auto value = keyValue(left);
  const bool rotation = track.target.kind == AnimationTargetKind::NodeRotation;
  if (upper != track.times.begin() && upper != track.times.end() &&
      track.interpolation != AnimationInterpolation::Step) {
    const double duration = track.times[left + 1] - track.times[left];
    const double t = (time - track.times[left]) / duration;
    auto next = keyValue(left + 1);
    if (cubic) {
      const double t2 = t * t, t3 = t2 * t;
      for (size_t c = 0; c < width; ++c)
        value[c] = (2*t3-3*t2+1)*value[c] + (t3-2*t2+t)*duration*track.values[left*stride+2*width+c]
                 + (-2*t3+3*t2)*next[c] + (t3-t2)*duration*track.values[(left+1)*stride+c];
    } else if (rotation) {
      normalizeQuaternion(value);
      normalizeQuaternion(next);
      double dot = 0;
      for (size_t c = 0; c < width; ++c) dot += value[c] * next[c];
      if (dot < 0) { for (auto& v : next) v = -v; dot = -dot; }
      dot = std::clamp(dot, 0.0, 1.0);
      double a = 1-t, b = t;
      if (dot < 0.9995) {
        const double angle = std::acos(dot), sine = std::sin(angle);
        a = std::sin((1-t)*angle)/sine; b = std::sin(t*angle)/sine;
      }
      for (size_t c = 0; c < width; ++c) value[c] = a*value[c] + b*next[c];
    } else for (size_t c = 0; c < width; ++c) value[c] = (1-t)*value[c] + t*next[c];
  }
  if (rotation) normalizeQuaternion(value);
  for (double v : value) if (!std::isfinite(v)) invalid("non-finite interpolated animation value");
  return value;
}
void uniqueAppend(std::vector<size_t>& target, size_t index) {
  if (std::find(target.begin(), target.end(), index) == target.end()) target.push_back(index);
}
double modularProduct(double delta, double speed, double duration) {
  int exponent = 0;
  const double mantissa = std::frexp(std::abs(speed), &exponent);
  double value = delta * mantissa; // mantissa <= 1; cannot overflow.
  if (exponent < 0) return std::fmod(std::ldexp(value, exponent), duration);
  value = std::fmod(value, duration);
  for (int i = 0; i < exponent; ++i)
    value = value >= duration - value ? value - (duration - value) : value + value;
  return value;
}
bool assignVector(std::vector<double>& destination, const AnimatedValue& sample,
                  const std::vector<double>& defaults = {}) {
  auto value = destination.empty() ? defaults : destination;
  if (sample.target.element) {
    if (value.size() != sample.target.components) invalid("animation target vector size changed");
    value[*sample.target.element] = sample.value[0];
  } else value = sample.value;
  if (value == destination) return false;
  destination = std::move(value);
  return true;
}
bool assignScalar(double& destination, const AnimatedValue& sample) {
  if (destination == sample.value[0]) return false;
  destination = sample.value[0];
  return true;
}
bool applyMaterial(tinygltf::Material& material, const AnimatedValue& sample) {
  const auto& property = sample.target.property;
  if (property == "pbrMetallicRoughness/baseColorFactor")
    return assignVector(material.pbrMetallicRoughness.baseColorFactor, sample, {1,1,1,1});
  if (property == "pbrMetallicRoughness/metallicFactor") return assignScalar(material.pbrMetallicRoughness.metallicFactor, sample);
  if (property == "pbrMetallicRoughness/roughnessFactor") return assignScalar(material.pbrMetallicRoughness.roughnessFactor, sample);
  if (property == "emissiveFactor") return assignVector(material.emissiveFactor, sample, {0,0,0});
  if (property == "alphaCutoff") return assignScalar(material.alphaCutoff, sample);
  if (property == "normalTexture/scale") return assignScalar(material.normalTexture.scale, sample);
  if (property == "occlusionTexture/strength") return assignScalar(material.occlusionTexture.strength, sample);
  const auto tokens = splitPointer('/' + property);
  auto& object = materialPropertyParent(material,tokens)->Get<tinygltf::Value::Object>();
  auto existing = object.find(tokens.back());
  if(sample.target.element) {
    const auto element=*sample.target.element;
    if(existing!=object.end()) {
      if(!existing->second.IsArray() || existing->second.ArrayLen()!=sample.target.components)
        invalid("invalid material animation vector property");
      auto& values=existing->second.Get<tinygltf::Value::Array>();
      if(values[element].IsNumber() && values[element].GetNumberAsDouble()==sample.value[0]) return false;
      // The animation owns this element only. Preserve every untracked raw
      // sibling, including integer types and signed-zero floating-point bits.
      values[element]=tinygltf::Value(sample.value[0]);
    } else {
      const double value=tokens.back()=="sheenColorFactor" || tokens.back()=="offset" ? 0. : 1.;
      tinygltf::Value::Array values(sample.target.components,tinygltf::Value(value));
      values[element]=tinygltf::Value(sample.value[0]);
      object.emplace(tokens.back(),tinygltf::Value(std::move(values)));
    }
    return true;
  }
  std::vector<double> value;
  if (sample.target.components == 1) value = sample.value;
  else {
    // Whole-vector channels retain the existing shape validation before
    // replacing their complete value. Component channels returned above.
    value.assign(sample.target.components, tokens.back() == "sheenColorFactor" || tokens.back() == "offset" ? 0.0 : 1.0);
    if (existing != object.end()) {
      if (!existing->second.IsArray() || existing->second.ArrayLen() != value.size())
        invalid("invalid material animation vector property");
      for (size_t i = 0; i < value.size(); ++i) value[i] = existing->second.Get(i).GetNumberAsDouble();
    }
    value = sample.value;
  }
  if (existing != object.end()) {
    bool same = value.size() == 1 ? existing->second.IsNumber() && existing->second.GetNumberAsDouble() == value[0]
                                : existing->second.IsArray() && existing->second.ArrayLen() == value.size();
    if (same && value.size() > 1) for (size_t i = 0; i < value.size(); ++i)
      same = same && existing->second.Get(i).IsNumber() && existing->second.Get(i).GetNumberAsDouble() == value[i];
    if (same) return false;
  }
  if (value.size() == 1) object.insert_or_assign(tokens.back(), tinygltf::Value(value[0]));
  else {
    tinygltf::Value::Array array;
    for (double v : value) array.emplace_back(v);
    object.insert_or_assign(tokens.back(), tinygltf::Value(std::move(array)));
  }
  return true;
}
} // namespace

bool AnimationDirtySet::empty() const {
  return nodes.empty() && weights.empty() && materials.empty() && lights.empty() && cameras.empty();
}
void AnimationDirtySet::merge(const AnimationDirtySet& other) {
  for (size_t v : other.nodes) uniqueAppend(nodes, v);
  for (size_t v : other.weights) uniqueAppend(weights, v);
  for (size_t v : other.materials) uniqueAppend(materials, v);
  for (size_t v : other.lights) uniqueAppend(lights, v);
  for (size_t v : other.cameras) uniqueAppend(cameras, v);
}
namespace {
AnimationTrack decodeAnimationTrack(const tinygltf::Model& model, const AnimationTarget& target,
    bool pointer, const tinygltf::AnimationSampler& sampler, size_t& remainingDecodedBytes) {
  const bool float64 = HasRequiredExtension(model,"KHR_accessor_float64");
  AnimationTrack track; track.target = target;
  if (sampler.interpolation.empty() || sampler.interpolation == "LINEAR") track.interpolation = AnimationInterpolation::Linear;
  else if (sampler.interpolation == "STEP") track.interpolation = AnimationInterpolation::Step;
  else if (sampler.interpolation == "CUBICSPLINE") track.interpolation = AnimationInterpolation::CubicSpline;
  else invalid("unsupported animation interpolation");
  const auto& input = checked(model.accessors, sampler.input, "input accessor");
  if (input.type != TINYGLTF_TYPE_SCALAR || (input.componentType != TINYGLTF_COMPONENT_TYPE_FLOAT && !(float64 && input.componentType == TINYGLTF_COMPONENT_TYPE_DOUBLE)) || input.normalized)
    invalid("animation input must be non-normalized FLOAT or declared DOUBLE SCALAR");
  track.times = readAccessor(model, sampler.input, remainingDecodedBytes);
  for (size_t i = 0; i < track.times.size(); ++i)
    if (track.times[i] < 0 || (i && track.times[i] <= track.times[i-1]))
      invalid("animation times must be nonnegative and strictly increasing");
  const auto inputPrecision = [&](double value) {
    return input.componentType == TINYGLTF_COMPONENT_TYPE_FLOAT ? static_cast<double>(static_cast<float>(value)) : value;
  };
  if (input.minValues.size() != 1 || input.maxValues.size() != 1 ||
      inputPrecision(input.minValues[0]) != track.times.front() || inputPrecision(input.maxValues[0]) != track.times.back())
    invalid("animation input min/max must match its first/last key");
  const bool cubic = track.interpolation == AnimationInterpolation::CubicSpline;
  if (cubic && track.times.size() < 2) invalid("CUBICSPLINE needs at least two keys");
  const auto& output = checked(model.accessors, sampler.output, "output accessor");
  const size_t width = sampleComponents(target);
  const size_t expectedComponents = target.kind == AnimationTargetKind::NodeWeights ? 1 : width;
  if (vectorComponents(output.type) != expectedComponents) invalid("animation output accessor shape mismatch");
  if (!pointer && (target.kind == AnimationTargetKind::NodeTranslation || target.kind == AnimationTargetKind::NodeScale)) {
    if ((output.componentType != TINYGLTF_COMPONENT_TYPE_FLOAT && !(float64 && output.componentType == TINYGLTF_COMPONENT_TYPE_DOUBLE)) || output.normalized)
      invalid("TRS translation/scale animation output must be FLOAT");
  } else if (!pointer && output.componentType != TINYGLTF_COMPONENT_TYPE_FLOAT &&
             !(float64 && output.componentType == TINYGLTF_COMPONENT_TYPE_DOUBLE) &&
             (!output.normalized || output.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT))
    invalid("rotation/weights animation integers must be normalized 8/16-bit");
  const size_t expected = product(product(track.times.size(), width), cubic ? 3 : 1);
  if (product(output.count, vectorComponents(output.type)) != expected) invalid("animation output accessor count mismatch");
  track.values = readAccessor(model, sampler.output, remainingDecodedBytes);
  if (target.kind == AnimationTargetKind::Material && extendedUnitIntervalProperty(target.property))
    for (size_t key = 0; key < track.times.size(); ++key) {
      const size_t first = (key*(cubic ? 3 : 1) + (cubic ? 1 : 0))*width;
      for (size_t c = 0; c < width; ++c)
        if (track.values[first+c] < 0 || track.values[first+c] > 1) invalid("extended material animation key is outside [0,1]");
    }
  if (target.kind == AnimationTargetKind::NodeRotation)
    for (size_t i = 0; i < track.times.size(); ++i) {
      const size_t offset = (i*(cubic ? 3 : 1) + (cubic ? 1 : 0))*4;
      std::vector<double> quaternion(track.values.begin()+offset, track.values.begin()+offset+4);
      normalizeQuaternion(quaternion);
    }
  return track;
}
AnimationLibrary parseAnimations(const tinygltf::Model& model, bool captureRests,
    size_t remainingDecodedBytes, bool checkOverlap = true) {
  AnimationLibrary library;
  if (captureRests) {
    library.restNodes = model.nodes; library.restMaterials = model.materials;
    library.restLights = model.lights; library.restCameras = model.cameras;
  }
  for (size_t clipIndex = 0; clipIndex < model.animations.size(); ++clipIndex) {
    const auto& source = model.animations[clipIndex];
    AnimationClip clip{source.name, 0.0, {}};
    for (size_t channelIndex = 0; channelIndex < source.channels.size(); ++channelIndex) {
      try {
        const auto& channel = source.channels[channelIndex];
        if (channel.target_node < -1) invalid("invalid animation target node index");
        const auto& sampler = checked(source.samplers, channel.sampler, "sampler");
        std::optional<AnimationTarget> target;
        const bool pointer = channel.target_path == "pointer";
        if (pointer) {
          if (channel.target_node != -1) invalid("pointer animation cannot also target a node");
          const auto extension = channel.target_extensions.find("KHR_animation_pointer");
          if (extension == channel.target_extensions.end() || !extension->second.IsObject() ||
              !extension->second.Get("pointer").IsString()) invalid("missing KHR_animation_pointer pointer");
          target = pointerTarget(model, extension->second.Get("pointer").Get<std::string>());
        } else if (channel.target_node >= 0) target = nodeTarget(model, static_cast<size_t>(channel.target_node), channel.target_path);
        if (!target) {
          library.diagnostics.push_back({clipIndex, channelIndex, "unsupported or undefined animation target omitted"});
          continue;
        }
        if (checkOverlap) for (const auto& existing : clip.tracks)
          if (overlapping(existing.target, *target)) invalid("duplicate/overlapping animation targets");
        auto track = decodeAnimationTrack(model, *target, pointer, sampler, remainingDecodedBytes);
        clip.duration = std::max(clip.duration, track.times.back());
        track.sourceChannel = channelIndex;
        clip.tracks.push_back(std::move(track));
      } catch (const std::invalid_argument& error) {
        invalid("animation " + std::to_string(clipIndex) + " channel " + std::to_string(channelIndex) + ": " + error.what());
      }
    }
    library.clips.push_back(std::move(clip));
  }
  return library;
}
} // namespace
AnimationLibrary ParseAnimations(const tinygltf::Model& model) {
  return parseAnimations(model, true, 128*1024*1024);
}
AnimationPose SampleAnimation(const AnimationClip& clip, double timeSeconds) {
  if (!std::isfinite(timeSeconds)) invalid("animation sample time must be finite");
  AnimationPose pose;
  for (const auto& track : clip.tracks) {
    const size_t expected = product(product(track.times.size(), sampleComponents(track.target)),
      track.interpolation == AnimationInterpolation::CubicSpline ? 3 : 1);
    if (track.times.empty() || !sampleComponents(track.target) || expected != track.values.size())
      invalid("invalid animation track storage");
    pose.values.push_back({track.target, sampleTrack(track, timeSeconds)});
  }
  return pose;
}
namespace {
// Only objects that this operation can touch are copied. Buffer/accessor data,
// meshes, images and unrelated source objects stay outside the transaction.
class AnimationTransaction {
public:
  static_assert(std::is_nothrow_swappable_v<tinygltf::Node> &&
                std::is_nothrow_swappable_v<tinygltf::Material> &&
                std::is_nothrow_swappable_v<tinygltf::Light> &&
                std::is_nothrow_swappable_v<tinygltf::Camera>);
  explicit AnimationTransaction(tinygltf::Model& model) : model_(model) {}
  AnimationTransaction(const AnimationTransaction&) = delete;
  AnimationTransaction& operator=(const AnimationTransaction&) = delete;
  ~AnimationTransaction() noexcept {
    if (committed_) return;
    for (auto& [index, value] : nodes_) std::swap(model_.nodes[index],value);
    for (auto& [index, value] : materials_) std::swap(model_.materials[index],value);
    for (auto& [index, value] : lights_) std::swap(model_.lights[index],value);
    for (auto& [index, value] : cameras_) std::swap(model_.cameras[index],value);
  }
  void capture(const AnimationTarget& target) {
    const auto copy = [&](auto& snapshots,const auto& source,const char* description) {
      if (target.index >= source.size()) invalid(std::string("animation pose ")+description+" index out of range");
      if (!snapshots.contains(target.index)) snapshots.emplace(target.index,source[target.index]);
    };
    switch (target.kind) {
      case AnimationTargetKind::Material: copy(materials_,model_.materials,"material"); break;
      case AnimationTargetKind::Light: copy(lights_,model_.lights,"light"); break;
      case AnimationTargetKind::Camera: copy(cameras_,model_.cameras,"camera"); break;
      case AnimationTargetKind::NodeTranslation: case AnimationTargetKind::NodeRotation:
      case AnimationTargetKind::NodeScale: case AnimationTargetKind::NodeWeights:
        copy(nodes_,model_.nodes,"node"); break;
      default: invalid("unsupported animation pose target kind");
    }
  }
  void capture(const AnimationPose& pose) { for (const auto& sample : pose.values) capture(sample.target); }
  void capture(const AnimationLibrary& library) {
    for (const auto& clip : library.clips) for (const auto& track : clip.tracks) capture(track.target);
  }
  const tinygltf::Material& materialBefore(size_t index) const {
    const auto found=materials_.find(index);
    if(found==materials_.end()) invalid("material animation cache has no rollback snapshot");
    return found->second;
  }
  void commit() noexcept { committed_ = true; }
private:
  tinygltf::Model& model_;
  std::map<size_t,tinygltf::Node> nodes_;
  std::map<size_t,tinygltf::Material> materials_;
  std::map<size_t,tinygltf::Light> lights_;
  std::map<size_t,tinygltf::Camera> cameras_;
  bool committed_{};
};
using MaterialCacheMasks=std::map<size_t,std::array<bool,6>>;
void restoreMaterialTarget(tinygltf::Material&,const tinygltf::Material&,const AnimationTarget&);
std::optional<size_t> materialCacheOwner(std::string_view);
void reconcileMaterialRestCaches(tinygltf::Material&,const tinygltf::Material&,
    const tinygltf::Material*,const std::array<bool,6>&);
void reconcileMaterialCaches(tinygltf::Model&,const MaterialCacheMasks&,
    const AnimationTransaction&,const AnimationLibrary*);
void markMaterialCache(MaterialCacheMasks& caches,const AnimationTarget& target) {
  if(const auto owner=materialCacheOwner(target.property)) caches[target.index][*owner]=true;
}
void validatePoseValues(const AnimationPose& pose) {
  for (const auto& sample : pose.values) {
    const auto& target = sample.target;
    if (!target.components) invalid("animation pose target has no components");
    if (sample.value.size() != sampleComponents(target)) invalid("animation pose component count mismatch");
    for (double v : sample.value) {
      if (!std::isfinite(v)) invalid("non-finite animation pose value");
      if (std::abs(v) > std::numeric_limits<float>::max())
        invalid("animation pose value is outside the runtime float range for " + target.property);
      const float runtime = static_cast<float>(v);
      if (v != 0 && runtime == 0)
        invalid("animation pose value is outside the runtime float range for " + target.property);
      if (target.kind == AnimationTargetKind::Material && extendedUnitIntervalProperty(target.property) && (v < 0 || v > 1))
        invalid("extended material animation pose value is outside [0,1] for " + target.property);
    }
    if (target.element && *target.element >= target.components) invalid("animation pose element out of range");
  }
}
AnimationDirtySet applyPose(tinygltf::Model& model, const AnimationPose& pose,MaterialCacheMasks& caches) {
  AnimationDirtySet dirty;
  for (const auto& sample : pose.values) {
    const auto& target = sample.target;
    bool changed = false;
    if (target.kind == AnimationTargetKind::Material) {
      if (target.index >= model.materials.size()) invalid("animation pose material index out of range");
      changed = applyMaterial(model.materials[target.index], sample);
      if(changed) markMaterialCache(caches,target);
      if (changed) uniqueAppend(dirty.materials, target.index);
    } else if (target.kind == AnimationTargetKind::Light) {
      if (target.index >= model.lights.size()) invalid("animation pose light index out of range");
      auto& light = model.lights[target.index];
      if (target.property == "color") changed = assignVector(light.color, sample, {1,1,1});
      else if (target.property == "intensity") changed = assignScalar(light.intensity, sample);
      else if (target.property == "range") changed = assignScalar(light.range, sample);
      else if (target.property == "spot/innerConeAngle") changed = assignScalar(light.spot.innerConeAngle, sample);
      else if (target.property == "spot/outerConeAngle") changed = assignScalar(light.spot.outerConeAngle, sample);
      else invalid("unsupported animation pose light property");
      if (changed) uniqueAppend(dirty.lights, target.index);
    } else if (target.kind == AnimationTargetKind::Camera) {
      if (target.index >= model.cameras.size()) invalid("animation pose camera index out of range");
      auto& camera = model.cameras[target.index];
      if (target.property == "perspective/yfov") changed = assignScalar(camera.perspective.yfov, sample);
      else if (target.property == "perspective/aspectRatio") changed = assignScalar(camera.perspective.aspectRatio, sample);
      else if (target.property == "perspective/znear") changed = assignScalar(camera.perspective.znear, sample);
      else if (target.property == "perspective/zfar") changed = assignScalar(camera.perspective.zfar, sample);
      else if (target.property == "orthographic/xmag") changed = assignScalar(camera.orthographic.xmag, sample);
      else if (target.property == "orthographic/ymag") changed = assignScalar(camera.orthographic.ymag, sample);
      else if (target.property == "orthographic/znear") changed = assignScalar(camera.orthographic.znear, sample);
      else if (target.property == "orthographic/zfar") changed = assignScalar(camera.orthographic.zfar, sample);
      else invalid("unsupported animation pose camera property");
      if (changed) uniqueAppend(dirty.cameras, target.index);
    } else {
      if (target.index >= model.nodes.size()) invalid("animation pose node index out of range");
      auto& node = model.nodes[target.index];
      if (target.kind == AnimationTargetKind::NodeTranslation) changed = assignVector(node.translation, sample, {0,0,0});
      else if (target.kind == AnimationTargetKind::NodeScale) changed = assignVector(node.scale, sample, {1,1,1});
      else if (target.kind == AnimationTargetKind::NodeRotation) changed = assignVector(node.rotation, sample, {0,0,0,1});
      else {
        const auto& mesh = checked(model.meshes, node.mesh, "morph mesh");
        changed = assignVector(node.weights, sample, mesh.weights.empty() ? std::vector<double>(target.components, 0.0) : mesh.weights);
      }
      if (changed) uniqueAppend(target.kind == AnimationTargetKind::NodeWeights ? dirty.weights : dirty.nodes, target.index);
    }
  }
  return dirty;
}
AnimationDirtySet restoreRestPose(tinygltf::Model& model, const AnimationLibrary& library,MaterialCacheMasks& caches) {
  if (model.nodes.size() != library.restNodes.size() || model.materials.size() != library.restMaterials.size() ||
      model.lights.size() != library.restLights.size() || model.cameras.size() != library.restCameras.size())
    invalid("animation source model structure changed");
  AnimationDirtySet dirty;
  // Material rests belong to the archive resource domain: restore only tracked
  // leaves/components so evaluated bindings and untracked edits stay live.
  // Copying exact authored node arrays restores
  // absent TRS/default weights as absent, rather than materializing defaults.
  for (const auto& clip : library.clips) for (const auto& track : clip.tracks) {
    const auto& target = track.target;
    if (target.kind == AnimationTargetKind::Material) {
      restoreMaterialTarget(model.materials[target.index],library.restMaterials[target.index],target);
      markMaterialCache(caches,target);
      uniqueAppend(dirty.materials, target.index);
    } else if (target.kind == AnimationTargetKind::Light) {
      model.lights[target.index] = library.restLights[target.index];
      uniqueAppend(dirty.lights, target.index);
    } else if (target.kind == AnimationTargetKind::Camera) {
      model.cameras[target.index] = library.restCameras[target.index];
      uniqueAppend(dirty.cameras, target.index);
    } else {
      auto& node = model.nodes[target.index]; const auto& rest = library.restNodes[target.index];
      if (target.kind == AnimationTargetKind::NodeTranslation) node.translation = rest.translation;
      else if (target.kind == AnimationTargetKind::NodeScale) node.scale = rest.scale;
      else if (target.kind == AnimationTargetKind::NodeRotation) node.rotation = rest.rotation;
      else node.weights = rest.weights;
      uniqueAppend(target.kind == AnimationTargetKind::NodeWeights ? dirty.weights : dirty.nodes, target.index);
    }
  }
  return dirty;
}
} // namespace

AnimationDirtySet ApplyAnimationPose(tinygltf::Model& model, const AnimationPose& pose) {
  validatePoseValues(pose); // Validate every channel before any source write.
  AnimationTransaction transaction(model); transaction.capture(pose);
  MaterialCacheMasks caches;
  auto dirty = applyPose(model,pose,caches);
  reconcileMaterialCaches(model,caches,transaction,nullptr);
  transaction.commit();
  return dirty;
}
AnimationDirtySet RestoreAnimationRestPose(tinygltf::Model& model, const AnimationLibrary& library) {
  AnimationTransaction transaction(model); transaction.capture(library);
  MaterialCacheMasks caches;
  auto dirty = restoreRestPose(model,library,caches);
  reconcileMaterialCaches(model,caches,transaction,&library);
  transaction.commit();
  return dirty;
}
AnimationPlayer::AnimationPlayer(const tinygltf::Model& model) : library_(ParseAnimations(model)) {}
namespace {
bool cloneRawValueEqual(const tinygltf::Value& a, const tinygltf::Value& b) {
  std::vector<std::pair<const tinygltf::Value*, const tinygltf::Value*>> pending{{&a, &b}};
  while (!pending.empty()) {
    const auto [left, right] = pending.back(); pending.pop_back();
    if (left->Type() != right->Type()) return false;
    if (left->IsInt() && left->Get<int>() != right->Get<int>()) return false;
    if (left->IsReal() && std::bit_cast<uint64_t>(left->Get<double>()) != std::bit_cast<uint64_t>(right->Get<double>())) return false;
    if (left->IsBool() && left->Get<bool>() != right->Get<bool>()) return false;
    if (left->IsString() && left->Get<std::string>() != right->Get<std::string>()) return false;
    if (left->IsBinary() && left->Get<std::vector<unsigned char>>() != right->Get<std::vector<unsigned char>>()) return false;
    if (left->IsArray()) {
      const auto& x = left->Get<tinygltf::Value::Array>(); const auto& y = right->Get<tinygltf::Value::Array>();
      if (x.size() != y.size()) return false;
      for (size_t i = 0; i < x.size(); ++i) pending.push_back({&x[i], &y[i]});
    } else if (left->IsObject()) {
      const auto& x = left->Get<tinygltf::Value::Object>(); const auto& y = right->Get<tinygltf::Value::Object>();
      if (x.size() != y.size()) return false;
      auto next = y.begin();
      for (const auto& [key, value] : x) {
        if (key != next->first) return false;
        pending.push_back({&value, &next->second}); ++next;
      }
    }
  }
  return true;
}
bool cloneRawExtensionsEqual(const tinygltf::ExtensionMap& a, const tinygltf::ExtensionMap& b) {
  if (a.size() != b.size()) return false;
  auto next = b.begin();
  for (const auto& [key, value] : a) {
    if (key != next->first || !cloneRawValueEqual(value, next->second)) return false;
    ++next;
  }
  return true;
}
template<class T> bool cloneRawMetadataEqual(const T& a, const T& b) {
  return cloneRawValueEqual(a.extras, b.extras) && cloneRawExtensionsEqual(a.extensions, b.extensions) &&
      a.extras_json_string == b.extras_json_string && a.extensions_json_string == b.extensions_json_string;
}
void restoreMaterialTarget(tinygltf::Material& material,const tinygltf::Material& rest,
    const AnimationTarget& target) {
  const auto vector=[&](std::vector<double>& out,const std::vector<double>& source) {
    if(source.size()!=target.components || out.size()!=target.components)
      invalid("material rest target vector size changed");
    for(const auto value:source) if(!std::isfinite(value)) invalid("nonfinite material rest value");
    if(target.element) out[*target.element]=source[*target.element];
    else out=source;
  };
  const auto scalar=[&](double& out,double source) {
    if(!std::isfinite(source)) invalid("nonfinite material rest value");out=source;
  };
  const auto& property=target.property;
  if(property=="pbrMetallicRoughness/baseColorFactor") {vector(material.pbrMetallicRoughness.baseColorFactor,rest.pbrMetallicRoughness.baseColorFactor);return;}
  if(property=="pbrMetallicRoughness/metallicFactor") {scalar(material.pbrMetallicRoughness.metallicFactor,rest.pbrMetallicRoughness.metallicFactor);return;}
  if(property=="pbrMetallicRoughness/roughnessFactor") {scalar(material.pbrMetallicRoughness.roughnessFactor,rest.pbrMetallicRoughness.roughnessFactor);return;}
  if(property=="emissiveFactor") {vector(material.emissiveFactor,rest.emissiveFactor);return;}
  if(property=="alphaCutoff") {scalar(material.alphaCutoff,rest.alphaCutoff);return;}
  if(property=="normalTexture/scale") {scalar(material.normalTexture.scale,rest.normalTexture.scale);return;}
  if(property=="occlusionTexture/strength") {scalar(material.occlusionTexture.strength,rest.occlusionTexture.strength);return;}
  const auto tokens=splitPointer('/'+property);
  const auto* source=materialPropertyParent(rest,tokens);
  auto* destination=materialPropertyParent(material,tokens);
  const auto& key=tokens.back();
  validateMaterialLeaf(*source,key,target.components,extendedUnitIntervalProperty(property));
  validateMaterialLeaf(*destination,key,target.components,extendedUnitIntervalProperty(property));
  auto& object=destination->Get<tinygltf::Value::Object>();
  if(!target.element) {
    if(source->Has(key)) object.insert_or_assign(key,source->Get(key));
    else object.erase(key); // Keep an omitted schema-default leaf omitted.
    return;
  }
  auto found=object.find(key);
  if(!source->Has(key) && found==object.end()) return;
  if(found==object.end()) {
    const double value=key=="sheenColorFactor" || key=="offset" ? 0. : 1.;
    tinygltf::Value::Array defaults(target.components,tinygltf::Value(value));
    found=object.emplace(key,tinygltf::Value(std::move(defaults))).first;
  }
  auto& values=found->second.Get<tinygltf::Value::Array>();
  // A component target must retain its supplied, untracked siblings, including
  // their raw numeric types. Only the targeted component uses authored rest.
  values[*target.element]=source->Has(key)?source->Get(key).Get(int(*target.element)):
      tinygltf::Value(key=="sheenColorFactor" || key=="offset" ? 0. : 1.);
  if(!source->Has(key)) {
    const auto defaultBits=std::bit_cast<uint64_t>(key=="sheenColorFactor" || key=="offset" ? 0. : 1.);
    // A sampled component can have materialized an otherwise omitted vector.
    // Return to omission only when no sibling carries distinct raw meaning.
    if(std::ranges::all_of(values,[&](const tinygltf::Value& value) {
      return value.IsReal() && std::bit_cast<uint64_t>(value.Get<double>())==defaultBits;
    })) object.erase(found);
  }
}
std::optional<size_t> materialCacheOwner(std::string_view property) {
  if(property.starts_with("extensions/")) return 0;
  constexpr std::array<std::string_view,5> textures{
      "pbrMetallicRoughness/baseColorTexture/", "pbrMetallicRoughness/metallicRoughnessTexture/",
      "normalTexture/", "occlusionTexture/", "emissiveTexture/"};
  if(property.find("/extensions/KHR_texture_transform/")!=std::string_view::npos)
    for(size_t i=0;i<textures.size();++i) if(property.starts_with(textures[i])) return i+1;
  return {};
}
void reconcileMaterialRestCaches(tinygltf::Material& result,const tinygltf::Material& original,
    const tinygltf::Material* rest,const std::array<bool,6>& caches) {
  const auto cache=[&](size_t index,auto& out,const auto& before,const auto* authored) {
    if(!caches[index] || cloneRawExtensionsEqual(out.extensions,before.extensions)) return;
    if(authored && cloneRawExtensionsEqual(out.extensions,authored->extensions)) out.extensions_json_string=authored->extensions_json_string;
    else out.extensions_json_string.clear();
  };
  cache(0,result,original,rest);
  cache(1,result.pbrMetallicRoughness.baseColorTexture,original.pbrMetallicRoughness.baseColorTexture,
      rest?&rest->pbrMetallicRoughness.baseColorTexture:nullptr);
  cache(2,result.pbrMetallicRoughness.metallicRoughnessTexture,original.pbrMetallicRoughness.metallicRoughnessTexture,
      rest?&rest->pbrMetallicRoughness.metallicRoughnessTexture:nullptr);
  cache(3,result.normalTexture,original.normalTexture,rest?&rest->normalTexture:nullptr);
  cache(4,result.occlusionTexture,original.occlusionTexture,rest?&rest->occlusionTexture:nullptr);
  cache(5,result.emissiveTexture,original.emissiveTexture,rest?&rest->emissiveTexture:nullptr);
}
void reconcileMaterialCaches(tinygltf::Model& model,const MaterialCacheMasks& caches,
    const AnimationTransaction& transaction,const AnimationLibrary* library) {
  // The existing rollback snapshot supplies Before; only six small flags per
  // affected material are new. Each affected extension map is compared once.
  for(const auto& [index,mask]:caches)
    reconcileMaterialRestCaches(model.materials[index],transaction.materialBefore(index),
        library?&library->restMaterials[index]:nullptr,mask);
}
} // namespace

void AnimationPlayer::setSpeed(double speed) {
  if (!std::isfinite(speed)) invalid("animation speed must be finite");
  speed_ = speed;
}
AnimationDirtySet AnimationPlayer::selectClip(tinygltf::Model& model, size_t clip, double timeSeconds) {
  if (clip >= library_.clips.size()) invalid("animation clip index out of range");
  if (!std::isfinite(timeSeconds)) invalid("animation seek time must be finite");
  const double next = std::clamp(timeSeconds, 0.0, library_.clips[clip].duration);
  const auto pose = SampleAnimation(library_.clips[clip], next);
  validatePoseValues(pose); // A failed selection must not restore the old clip first.
  AnimationTransaction transaction(model); transaction.capture(library_);
  MaterialCacheMasks caches;
  auto dirty = restoreRestPose(model,library_,caches);
  dirty.merge(applyPose(model,pose,caches));
  reconcileMaterialCaches(model,caches,transaction,&library_);
  transaction.commit();
  selected_ = clip; time_ = next;
  return dirty;
}
AnimationDirtySet AnimationPlayer::clearClip(tinygltf::Model& model) {
  auto dirty = RestoreAnimationRestPose(model, library_);
  selected_.reset(); time_ = 0; playing_ = false;
  return dirty;
}
AnimationDirtySet AnimationPlayer::seek(tinygltf::Model& model, double timeSeconds) {
  if (!std::isfinite(timeSeconds)) invalid("animation seek time must be finite");
  if (!selected_) return {};
  const auto& clip = library_.clips[*selected_];
  // Seeking the duration deliberately displays the last key, including loops.
  const double next = std::clamp(timeSeconds,0.0,clip.duration);
  auto dirty = ApplyAnimationPose(model,SampleAnimation(clip,next));
  time_ = next;
  return dirty;
}
AnimationDirtySet AnimationPlayer::advance(tinygltf::Model& model, double deltaSeconds) {
  if (!std::isfinite(deltaSeconds) || deltaSeconds < 0) invalid("animation delta must be finite and nonnegative");
  if (!playing_ || !selected_ || deltaSeconds == 0 || speed_ == 0) return {};
  const auto& clip = library_.clips[*selected_];
  if (clip.duration <= 0) {
    auto dirty = seek(model,0); playing_ = false; return dirty;
  }
  double next;
  bool nextPlaying = playing_;
  if (loop_) {
    // MSVC long double has double's range. Modular doubling avoids overflow
    // even when delta*speed or time+delta would exceed that range.
    const double duration = clip.duration;
    const double delta = modularProduct(deltaSeconds, speed_, duration);
    const double base = std::fmod(time_, duration);
    if (speed_ > 0) next = base >= duration - delta ? base - (duration - delta) : base + delta;
    else next = base >= delta ? base - delta : duration - (delta - base);
  } else {
    const double remaining = speed_ > 0 ? clip.duration - time_ : time_;
    if (deltaSeconds >= remaining / std::abs(speed_)) { next = speed_ > 0 ? clip.duration : 0; nextPlaying = false; }
    else next = time_ + deltaSeconds * speed_;
  }
  auto dirty = seek(model,next);
  playing_ = nextPlaying;
  return dirty;
}
} // namespace container::geometry::gltf
