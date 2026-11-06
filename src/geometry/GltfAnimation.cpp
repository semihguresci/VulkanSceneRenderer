#include <Container/geometry/GltfAnimation.h>
#include <Container/geometry/GltfAccessor.h>
#include <Container/geometry/GltfSubtreeClone.h>
#include <Container/geometry/GltfProjectedSubtreeClone.h>

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
AnimationPlayer AnimationPlayer::withExtendedSource(const tinygltf::Model& source) const {
  if (source.nodes.size() < library_.restNodes.size() ||
      source.materials.size() < library_.restMaterials.size() ||
      source.lights.size() < library_.restLights.size() ||
      source.cameras.size() < library_.restCameras.size() ||
      source.animations.size() < library_.clips.size())
    invalid("animation source extension removed existing identities");
  AnimationPlayer prepared(source);
  if (!std::equal(library_.clips.begin(), library_.clips.end(),
                  prepared.library_.clips.begin()))
    invalid("animation source extension changed an existing clip");
  // The source may already contain A's evaluated pose. Preserve its original
  // rest data rather than treating that pose as authored input. The snapshots
  // for newly appended objects remain those parsed from B's authored source.
  std::copy(library_.restNodes.begin(), library_.restNodes.end(),
            prepared.library_.restNodes.begin());
  std::copy(library_.restMaterials.begin(), library_.restMaterials.end(),
            prepared.library_.restMaterials.begin());
  std::copy(library_.restLights.begin(), library_.restLights.end(),
            prepared.library_.restLights.begin());
  std::copy(library_.restCameras.begin(), library_.restCameras.end(),
            prepared.library_.restCameras.begin());
  prepared.selected_ = selected_;
  prepared.time_ = time_;
  prepared.speed_ = speed_;
  prepared.playing_ = playing_;
  prepared.loop_ = loop_;
  return prepared;
}
namespace {
// TinyGLTF's equality omits raw target metadata and uses approximate numeric
// comparisons. Publication proofs require the original typed/raw representation.
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
bool cloneRawChannelEqual(const tinygltf::AnimationChannel& a, const tinygltf::AnimationChannel& b) {
  return a.sampler == b.sampler && a.target_node == b.target_node && a.target_path == b.target_path &&
      cloneRawMetadataEqual(a, b) && cloneRawValueEqual(a.target_extras, b.target_extras) &&
      cloneRawExtensionsEqual(a.target_extensions, b.target_extensions) &&
      a.target_extras_json_string == b.target_extras_json_string &&
      a.target_extensions_json_string == b.target_extensions_json_string;
}
bool cloneDoubleValuesEqual(const std::vector<double>& a, const std::vector<double>& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (std::bit_cast<uint64_t>(a[i]) != std::bit_cast<uint64_t>(b[i])) return false;
  return true;
}
bool cloneTrackEqual(const AnimationTrack& a, const AnimationTrack& b, const AnimationTarget& target) {
  return b.target == target && a.interpolation == b.interpolation &&
      cloneDoubleValuesEqual(a.times, b.times) && cloneDoubleValuesEqual(a.values, b.values);
}
bool cloneNodeTarget(AnimationTargetKind kind) {
  return kind == AnimationTargetKind::NodeTranslation || kind == AnimationTargetKind::NodeRotation ||
      kind == AnimationTargetKind::NodeScale || kind == AnimationTargetKind::NodeWeights;
}
tinygltf::AnimationChannel cloneRemappedChannel(const tinygltf::AnimationChannel& original,
    size_t sourceNode, int32_t cloneNode) {
  auto channel = original;
  if (channel.target_path != "pointer") {
    if (channel.target_node < 0 || size_t(channel.target_node) != sourceNode)
      invalid("animation clone original node identity changed");
    channel.target_node = cloneNode;
  } else {
    auto extension = channel.target_extensions.find("KHR_animation_pointer");
    if (extension == channel.target_extensions.end() || !extension->second.IsObject() ||
        !extension->second.Has("pointer") || !extension->second.Get("pointer").IsString())
      invalid("animation clone node pointer is missing");
    const auto& old = extension->second.Get("pointer").Get<std::string>();
    const auto prefix = "/nodes/" + std::to_string(sourceNode) + "/";
    if (!old.starts_with(prefix)) invalid("animation clone pointer does not match its typed source node");
    const auto pointer = "/nodes/" + std::to_string(cloneNode) + old.substr(prefix.size() - 1);
    extension->second.Get<tinygltf::Value::Object>()["pointer"] = tinygltf::Value(pointer);
    channel.target_extensions_json_string.clear();
  }
  return channel;
}

// Exact field comparison for the resident TinyGLTF resource types used below.
// In particular, its built-in equality ignores some sparse/raw metadata and
// treats nearby doubles as equal. Never use it as a publication witness.
template<class T> bool projectedExact(const T& a, const T& b) {
  if constexpr (std::is_same_v<T, tinygltf::Value>) return cloneRawValueEqual(a,b);
  else if constexpr (std::is_floating_point_v<T>) {
    static_assert(std::is_same_v<T,double>);
    return std::bit_cast<uint64_t>(a)==std::bit_cast<uint64_t>(b);
  } else if constexpr (std::is_arithmetic_v<T> || std::is_enum_v<T> || std::is_same_v<T,std::string>) return a==b;
  else if constexpr (requires { typename T::mapped_type; }) {
    if(a.size()!=b.size()) return false;
    auto next=b.begin();
    for(const auto& [key,value]:a) {
      if(key!=next->first || !projectedExact(value,next->second)) return false;
      ++next;
    }
    return true;
  } else if constexpr (requires { typename T::value_type; a.begin(); a.end(); }) {
    if(a.size()!=b.size()) return false;
    for(size_t i=0;i<a.size();++i) if(!projectedExact(a[i],b[i])) return false;
    return true;
  } else {
    static_assert(requires { a.extras; } || std::is_same_v<T,tinygltf::Parameter>);
#define CLONE_EXACT_FIELD(field) if constexpr(requires { a.field; }) if(!projectedExact(a.field,b.field)) return false
    CLONE_EXACT_FIELD(extras); CLONE_EXACT_FIELD(extensions);
    CLONE_EXACT_FIELD(extras_json_string); CLONE_EXACT_FIELD(extensions_json_string);
    CLONE_EXACT_FIELD(name); CLONE_EXACT_FIELD(data); CLONE_EXACT_FIELD(uri);
    CLONE_EXACT_FIELD(width); CLONE_EXACT_FIELD(height); CLONE_EXACT_FIELD(component); CLONE_EXACT_FIELD(bits);
    CLONE_EXACT_FIELD(pixel_type); CLONE_EXACT_FIELD(image); CLONE_EXACT_FIELD(mimeType); CLONE_EXACT_FIELD(as_is);
    CLONE_EXACT_FIELD(buffer); CLONE_EXACT_FIELD(bufferView); CLONE_EXACT_FIELD(byteOffset);
    CLONE_EXACT_FIELD(byteLength); CLONE_EXACT_FIELD(byteStride); CLONE_EXACT_FIELD(target);
    CLONE_EXACT_FIELD(dracoDecoded); CLONE_EXACT_FIELD(normalized); CLONE_EXACT_FIELD(componentType);
    CLONE_EXACT_FIELD(count); CLONE_EXACT_FIELD(type); CLONE_EXACT_FIELD(minValues); CLONE_EXACT_FIELD(maxValues);
    CLONE_EXACT_FIELD(minFilter); CLONE_EXACT_FIELD(magFilter); CLONE_EXACT_FIELD(wrapS); CLONE_EXACT_FIELD(wrapT);
    CLONE_EXACT_FIELD(sampler); CLONE_EXACT_FIELD(source); CLONE_EXACT_FIELD(index); CLONE_EXACT_FIELD(texCoord);
    CLONE_EXACT_FIELD(scale); CLONE_EXACT_FIELD(strength); CLONE_EXACT_FIELD(baseColorFactor);
    CLONE_EXACT_FIELD(baseColorTexture); CLONE_EXACT_FIELD(metallicFactor); CLONE_EXACT_FIELD(roughnessFactor);
    CLONE_EXACT_FIELD(metallicRoughnessTexture); CLONE_EXACT_FIELD(emissiveFactor); CLONE_EXACT_FIELD(alphaMode);
    CLONE_EXACT_FIELD(alphaCutoff); CLONE_EXACT_FIELD(doubleSided); CLONE_EXACT_FIELD(lods);
    CLONE_EXACT_FIELD(pbrMetallicRoughness); CLONE_EXACT_FIELD(normalTexture); CLONE_EXACT_FIELD(occlusionTexture);
    CLONE_EXACT_FIELD(emissiveTexture); CLONE_EXACT_FIELD(values); CLONE_EXACT_FIELD(additionalValues);
    CLONE_EXACT_FIELD(bool_value); CLONE_EXACT_FIELD(has_number_value); CLONE_EXACT_FIELD(string_value);
    CLONE_EXACT_FIELD(number_array); CLONE_EXACT_FIELD(json_double_value); CLONE_EXACT_FIELD(number_value);
    CLONE_EXACT_FIELD(attributes); CLONE_EXACT_FIELD(material); CLONE_EXACT_FIELD(indices);
    CLONE_EXACT_FIELD(mode); CLONE_EXACT_FIELD(targets); CLONE_EXACT_FIELD(primitives); CLONE_EXACT_FIELD(weights);
    CLONE_EXACT_FIELD(inverseBindMatrices); CLONE_EXACT_FIELD(skeleton); CLONE_EXACT_FIELD(joints);
    CLONE_EXACT_FIELD(camera); CLONE_EXACT_FIELD(skin); CLONE_EXACT_FIELD(mesh); CLONE_EXACT_FIELD(light);
    CLONE_EXACT_FIELD(emitter); CLONE_EXACT_FIELD(children); CLONE_EXACT_FIELD(rotation);
    CLONE_EXACT_FIELD(translation); CLONE_EXACT_FIELD(matrix); CLONE_EXACT_FIELD(perspective); CLONE_EXACT_FIELD(orthographic);
    CLONE_EXACT_FIELD(aspectRatio); CLONE_EXACT_FIELD(yfov); CLONE_EXACT_FIELD(zfar); CLONE_EXACT_FIELD(znear);
    CLONE_EXACT_FIELD(xmag); CLONE_EXACT_FIELD(ymag); CLONE_EXACT_FIELD(color); CLONE_EXACT_FIELD(intensity);
    CLONE_EXACT_FIELD(range); CLONE_EXACT_FIELD(spot); CLONE_EXACT_FIELD(innerConeAngle); CLONE_EXACT_FIELD(outerConeAngle);
    CLONE_EXACT_FIELD(version); CLONE_EXACT_FIELD(generator); CLONE_EXACT_FIELD(minVersion); CLONE_EXACT_FIELD(copyright);
    CLONE_EXACT_FIELD(isSparse);
#undef CLONE_EXACT_FIELD
    if constexpr(requires { a.sparse; }) {
      if(a.sparse.isSparse!=b.sparse.isSparse || !cloneRawMetadataEqual(a.sparse,b.sparse) ||
          !cloneRawMetadataEqual(a.sparse.indices,b.sparse.indices) ||
          !cloneRawMetadataEqual(a.sparse.values,b.sparse.values)) return false;
      // TinyGLTF leaves the inactive sparse numeric fields uninitialized.
      if(a.sparse.isSparse && !projectedExact(a.sparse,b.sparse)) return false;
    }
    return true;
  }
}

template<class T> void projectedPrefix(const std::vector<T>& current,const std::vector<T>& output,
    const char* description,bool suffix=false) {
  if((suffix?current.size()>output.size():current.size()!=output.size())) invalid(description);
  for(size_t i=0;i<current.size();++i) if(!projectedExact(current[i],output[i])) invalid(description);
}

void projectedResources(const tinygltf::Model& current,const tinygltf::Model& output) {
  projectedPrefix(current.accessors,output.accessors,"projected clone changed accessor identity",true);
  projectedPrefix(current.bufferViews,output.bufferViews,"projected clone changed buffer-view identity",true);
  projectedPrefix(current.samplers,output.samplers,"projected clone changed sampler identity",true);
  projectedPrefix(current.textures,output.textures,"projected clone changed texture identity",true);
  projectedPrefix(current.meshes,output.meshes,"projected clone changed mesh identity");
  projectedPrefix(current.skins,output.skins,"projected clone changed retained skin identity",true);
  if(current.buffers.size()>output.buffers.size() || current.images.size()>output.images.size())
    invalid("projected clone removed resident resource identities");
  for(size_t i=0;i<current.buffers.size();++i) {
    const auto& a=current.buffers[i];const auto& b=output.buffers[i];
    // Archive normalization may change a resident resource's URI spelling.
    if(a.name!=b.name || a.data!=b.data || !cloneRawMetadataEqual(a,b))
      invalid("projected clone changed resident buffer bytes or metadata");
  }
  for(size_t i=0;i<current.images.size();++i) {
    const auto& a=current.images[i];const auto& b=output.images[i];
    if(a.name!=b.name || !cloneRawMetadataEqual(a,b)) invalid("projected clone changed image provenance");
    // The archive can retain compressed bytes while the live source contains
    // decoded pixels. Within a matching representation, the payload is exact.
    const bool comparable=a.as_is==b.as_is && a.mimeType==b.mimeType && a.width==b.width &&
        a.height==b.height && a.component==b.component && a.bits==b.bits && a.pixel_type==b.pixel_type;
    if(comparable && (a.bufferView!=b.bufferView || a.image!=b.image))
      invalid("projected clone changed image bytes within the same representation");
  }
}

bool projectedNodePayloadEqual(const tinygltf::Node& original,const tinygltf::Node& next,
    GltfCloneNodeRole role,bool binding) {
  auto expected=original;
  // These identities/edges and the local flag values have already been checked
  // against the immutable graph projection. All other metadata remains exact.
  expected.name=next.name;expected.children=next.children;expected.skin=next.skin;
  if(role!=GltfCloneNodeRole::Visual) {
    expected.camera=expected.light=expected.emitter=-1;
    for(const auto* extension:{"KHR_lights_punctual","KHR_lights_area","EXT_lights_area","WEBGI_lights_area","WEBGI_area_light"})
      expected.extensions.erase(extension);
  }
  if(!binding) {expected.mesh=expected.skin=-1;expected.weights.clear();expected.extensions.erase("EXT_mesh_gpu_instancing");}
  const auto flag=[&](const char* extension,const char* property) {
    const auto target=next.extensions.find(extension);
    const bool enabled=target==next.extensions.end() || !target->second.Has(property) || target->second.Get(property).Get<bool>();
    auto old=expected.extensions.find(extension);
    if(old==expected.extensions.end()) {
      if(enabled) return;
      old=expected.extensions.emplace(extension,tinygltf::Value(tinygltf::Value::Object{})).first;
    }
    if(!old->second.IsObject()) invalid("projected animation clone has malformed source node flags");
    auto& object=old->second.Get<tinygltf::Value::Object>();const auto value=object.find(property);
    if(!enabled || (value!=object.end() && (!value->second.IsBool() || value->second.Get<bool>()!=enabled)))
      object[property]=tinygltf::Value(enabled);
  };
  flag("KHR_node_visibility","visible");flag("KHR_node_selectability","selectable");flag("KHR_node_hoverability","hoverable");
  expected.extensions_json_string.clear();
  return projectedExact(expected,next);
}
} // namespace

AnimationPlayer AnimationPlayer::withClonedSource(const tinygltf::Model& current,
    const tinygltf::Model& source, const GltfSubtreeClonePlan& clone) const {
  const auto oldNodes = library_.restNodes.size();
  if (!library_.diagnostics.empty() || current.nodes.size() != oldNodes || source.nodes.size() <= oldNodes ||
      current.materials.size() != library_.restMaterials.size() || source.materials.size() != library_.restMaterials.size() ||
      current.lights.size() != library_.restLights.size() || source.lights.size() != library_.restLights.size() ||
      current.cameras.size() != library_.restCameras.size() || source.cameras.size() != library_.restCameras.size() ||
      current.animations.size() != library_.clips.size() || source.animations.size() != library_.clips.size() ||
      clone.nodes.size() != oldNodes || clone.contextOnly.size() != oldNodes ||
      clone.originalChannelCounts.size() != library_.clips.size())
    invalid("animation clone changed existing source counts or unsupported clips");
  std::vector<bool> covered(source.nodes.size() - oldNodes);
  for (size_t n = 0; n < oldNodes; ++n) {
    const auto mapped = clone.nodes[n];
    if (mapped == -1) {
      if (clone.contextOnly[n]) invalid("animation clone context has no mapped node");
      continue;
    }
    if (mapped < 0 || size_t(mapped) < oldNodes || size_t(mapped) >= source.nodes.size() ||
        covered[size_t(mapped) - oldNodes]) invalid("animation clone map is not injective into the appended tail");
    covered[size_t(mapped) - oldNodes] = true;
    if (clone.contextOnly[n]) {
      const auto& context = source.nodes[size_t(mapped)];
      if (context.mesh != -1 || context.skin != -1 || context.camera != -1 || context.light != -1 ||
          context.emitter != -1 || !context.weights.empty())
        invalid("animation clone context retains visual attachments or morph weights");
    }
  }
  if (std::ranges::find(covered, false) != covered.end())
    invalid("animation clone map does not cover every appended node");
  // Validate the raw prefix independently of the decoded representation and
  // before making any authored-rest copies. A mutable public plan is a witness,
  // never authority to replace a current clip or add a resource-target track.
  for (size_t i = 0; i < library_.clips.size(); ++i) {
    const auto& old = current.animations[i]; const auto& next = source.animations[i];
    if (clone.originalChannelCounts[i] != old.channels.size() || old.channels.size() != library_.clips[i].tracks.size() ||
        next.channels.size() < old.channels.size() || next.samplers.size() != old.samplers.size() ||
        old.name != next.name || !cloneRawMetadataEqual(old, next))
      invalid("animation clone changed the original raw clip prefix");
    for (size_t sampler = 0; sampler < old.samplers.size(); ++sampler) {
      const auto& a = old.samplers[sampler]; const auto& b = next.samplers[sampler];
      if (a.input != b.input || a.output != b.output || a.interpolation != b.interpolation || !cloneRawMetadataEqual(a, b))
        invalid("animation clone changed an original sampler");
    }
    for (size_t c = 0; c < old.channels.size(); ++c)
      if (!cloneRawChannelEqual(old.channels[c], next.channels[c]))
        invalid("animation clone changed an original channel");
  }
  AnimationPlayer prepared(source);
  if (!prepared.library_.diagnostics.empty()) invalid("animation clone contains unsupported channels");
  for (size_t i = 0; i < library_.clips.size(); ++i) {
    const auto& old = library_.clips[i]; const auto& next = prepared.library_.clips[i];
    const auto& oldRaw = current.animations[i]; const auto& nextRaw = source.animations[i];
    if (old.name != next.name || std::bit_cast<uint64_t>(old.duration) != std::bit_cast<uint64_t>(next.duration))
      invalid("animation clone changed an existing decoded clip");
    for (size_t c = 0; c < old.tracks.size(); ++c)
      if (!cloneTrackEqual(old.tracks[c], next.tracks[c], old.tracks[c].target))
        invalid("animation clone changed an existing decoded track");
    auto suffix = old.tracks.size();
    for (size_t c = 0; c < old.tracks.size(); ++c) {
      const auto& track = old.tracks[c];
      if (!cloneNodeTarget(track.target.kind)) continue;
      if (track.target.index >= oldNodes) invalid("animation clone original target is invalid");
      const auto mapped = clone.nodes[track.target.index];
      if (mapped < 0 || (clone.contextOnly[track.target.index] && track.target.kind == AnimationTargetKind::NodeWeights)) continue;
      if (suffix >= next.tracks.size() || suffix >= nextRaw.channels.size())
        invalid("animation clone remapped channel is missing");
      auto target = track.target; target.index = size_t(mapped);
      if (!cloneTrackEqual(track, next.tracks[suffix], target) ||
          !cloneRawChannelEqual(cloneRemappedChannel(oldRaw.channels[c], track.target.index, mapped), nextRaw.channels[suffix]))
        invalid("animation clone suffix is not the declared node remap");
      ++suffix;
    }
    if (suffix != next.tracks.size() || suffix != nextRaw.channels.size())
      invalid("animation clone has undeclared channel suffixes");
  }
  std::copy(library_.restNodes.begin(), library_.restNodes.end(), prepared.library_.restNodes.begin());
  for (size_t n = 0; n < oldNodes; ++n) if (clone.nodes[n] >= 0) {
    auto& rest = prepared.library_.restNodes[size_t(clone.nodes[n])];
    const auto& original = library_.restNodes[n];
    rest.translation = original.translation; rest.rotation = original.rotation; rest.scale = original.scale;
    if (clone.contextOnly[n]) rest.weights.clear();
    else rest.weights = original.weights;
  }
  std::copy(library_.restMaterials.begin(), library_.restMaterials.end(), prepared.library_.restMaterials.begin());
  std::copy(library_.restLights.begin(), library_.restLights.end(), prepared.library_.restLights.begin());
  std::copy(library_.restCameras.begin(), library_.restCameras.end(), prepared.library_.restCameras.begin());
  prepared.selected_ = selected_; prepared.time_ = time_; prepared.speed_ = speed_;
  prepared.playing_ = playing_; prepared.loop_ = loop_;
  return prepared;
}

AnimationPlayer AnimationPlayer::withClonedSource(const tinygltf::Model& current,
    const GltfProjectedSubtreeClonePlan& projected,const GltfProjectedCloneOwnershipProof& ownership) const {
  ownership.validate(current,projected);
  const auto& clone=projected.clone;const auto& source=clone.evaluatedSource;
  const auto oldNodes=library_.restNodes.size();
  if(!library_.diagnostics.empty() || current.nodes.size()!=oldNodes || source.nodes.size()<oldNodes ||
      current.materials.size()!=library_.restMaterials.size() || source.materials.size()!=library_.restMaterials.size() ||
      current.lights.size()!=library_.restLights.size() || source.lights.size()!=library_.restLights.size() ||
      current.cameras.size()!=library_.restCameras.size() || source.cameras.size()!=library_.restCameras.size() ||
      current.animations.size()!=library_.clips.size() || source.animations.size()!=library_.clips.size() ||
      clone.originalChannelCounts.size()!=library_.clips.size())
    invalid("projected animation clone changed existing source counts or unsupported clips");
  for(const auto* output:{&clone.authoredSource,&source}) {
    projectedResources(current,*output);
    if(!projectedExact(current.asset,output->asset) || !cloneRawMetadataEqual(current,*output) ||
        current.extensionsRequired!=output->extensionsRequired)
      invalid("projected animation clone changed source metadata");
    for(const auto& name:current.extensionsUsed)
      if(std::ranges::find(output->extensionsUsed,name)==output->extensionsUsed.end())
        invalid("projected animation clone removed an extension declaration");
    for(const auto& name:output->extensionsUsed)
      if(std::ranges::find(current.extensionsUsed,name)==current.extensionsUsed.end() &&
          name!="KHR_node_visibility" && name!="KHR_node_selectability" && name!="KHR_node_hoverability")
        invalid("projected animation clone invented an extension declaration");
  }
  // Both outputs share the archive's complete resource tables, including any
  // pre-existing archive-only suffixes. Their typed evaluated prefixes retain
  // the current pose; the authored output can legitimately have different rests.
  const auto& authored=clone.authoredSource;
  if(authored.animations.size()!=source.animations.size() ||
      !projectedExact(authored.buffers,source.buffers) || !projectedExact(authored.bufferViews,source.bufferViews) ||
      !projectedExact(authored.accessors,source.accessors) || !projectedExact(authored.images,source.images) ||
      !projectedExact(authored.samplers,source.samplers) || !projectedExact(authored.textures,source.textures) ||
      !projectedExact(authored.meshes,source.meshes) || !projectedExact(authored.skins,source.skins) ||
      source.defaultScene!=current.defaultScene || !current.audioEmitters.empty() || !current.audioSources.empty() ||
      !source.audioEmitters.empty() || !source.audioSources.empty() ||
      !authored.audioEmitters.empty() || !authored.audioSources.empty())
    invalid("projected animation clone changed shared resource tables");
  projectedPrefix(current.nodes,source.nodes,"projected animation clone changed retained node payload",true);
  projectedPrefix(current.materials,source.materials,"projected animation clone changed evaluated materials");
  projectedPrefix(current.lights,source.lights,"projected animation clone changed evaluated lights");
  projectedPrefix(current.cameras,source.cameras,"projected animation clone changed evaluated cameras");
  for(size_t n=0;n<oldNodes;++n) if(clone.nodes[n]>=0) {
    const auto& old=current.nodes[n];const auto& next=source.nodes[size_t(clone.nodes[n])];
    if(!projectedNodePayloadEqual(old,next,projected.roles[n],projected.retainMeshBindings[n]))
      invalid("projected animation clone changed the mapped evaluated payload");
    const auto& rest=authored.nodes[n];
    if(!projectedNodePayloadEqual(rest,authored.nodes[size_t(clone.nodes[n])],projected.roles[n],projected.retainMeshBindings[n]))
      invalid("projected animation clone changed the mapped authored payload");
  }
  // Validate raw channels before decoding. Resource-target channels stay shared;
  // only explicitly owned node/weight tracks may form an ordered suffix.
  for(size_t i=0;i<library_.clips.size();++i) {
    const auto& old=current.animations[i];const auto& next=source.animations[i];
    if(clone.originalChannelCounts[i]!=old.channels.size() || old.channels.size()!=library_.clips[i].tracks.size() ||
        next.channels.size()<old.channels.size() || next.samplers.size()!=old.samplers.size() ||
        old.name!=next.name || !cloneRawMetadataEqual(old,next))
      invalid("projected animation clone changed the original raw clip prefix");
    for(size_t s=0;s<old.samplers.size();++s) {
      const auto& a=old.samplers[s];const auto& b=next.samplers[s];
      if(a.input!=b.input || a.output!=b.output || a.interpolation!=b.interpolation || !cloneRawMetadataEqual(a,b))
        invalid("projected animation clone changed an original sampler");
    }
    for(size_t c=0;c<old.channels.size();++c)
      if(!cloneRawChannelEqual(old.channels[c],next.channels[c]))
        invalid("projected animation clone changed an original channel");
    const auto& archived=authored.animations;
    if(archived.size()!=source.animations.size() || archived[i].name!=next.name ||
        !cloneRawMetadataEqual(archived[i],next) || archived[i].samplers.size()!=next.samplers.size() ||
        archived[i].channels.size()!=next.channels.size()) invalid("projected animation clone changed authored clips");
    for(size_t s=0;s<next.samplers.size();++s) {
      const auto& a=archived[i].samplers[s];const auto& b=next.samplers[s];
      if(a.input!=b.input || a.output!=b.output || a.interpolation!=b.interpolation || !cloneRawMetadataEqual(a,b))
        invalid("projected animation clone changed authored samplers");
    }
    for(size_t c=0;c<next.channels.size();++c)
      if(!cloneRawChannelEqual(archived[i].channels[c],next.channels[c]))
        invalid("projected animation clone changed authored channels");
  }
  AnimationPlayer prepared(source);
  if(!prepared.library_.diagnostics.empty()) invalid("projected animation clone contains unsupported channels");
  for(size_t i=0;i<library_.clips.size();++i) {
    const auto& old=library_.clips[i];const auto& next=prepared.library_.clips[i];
    const auto& oldRaw=current.animations[i];const auto& nextRaw=source.animations[i];
    if(old.name!=next.name || std::bit_cast<uint64_t>(old.duration)!=std::bit_cast<uint64_t>(next.duration) ||
        next.tracks.size()<old.tracks.size()) invalid("projected animation clone changed an existing decoded clip");
    for(size_t c=0;c<old.tracks.size();++c)
      if(!cloneTrackEqual(old.tracks[c],next.tracks[c],old.tracks[c].target))
        invalid("projected animation clone changed an existing decoded track");
    auto suffix=old.tracks.size();
    for(size_t c=0;c<old.tracks.size();++c) {
      const auto& track=old.tracks[c];if(!cloneNodeTarget(track.target.kind)) continue;
      if(track.target.index>=oldNodes) invalid("projected animation clone original target is invalid");
      const auto mapped=clone.nodes[track.target.index];
      if(mapped<0 || (track.target.kind==AnimationTargetKind::NodeWeights && !projected.copyWeightTracks[track.target.index])) continue;
      if(suffix>=next.tracks.size() || suffix>=nextRaw.channels.size())
        invalid("projected animation clone remapped channel is missing");
      auto target=track.target;target.index=size_t(mapped);
      if(!cloneTrackEqual(track,next.tracks[suffix],target) ||
          !cloneRawChannelEqual(cloneRemappedChannel(oldRaw.channels[c],track.target.index,mapped),nextRaw.channels[suffix]))
        invalid("projected animation clone suffix is not the declared node remap");
      ++suffix;
    }
    if(suffix!=next.tracks.size() || suffix!=nextRaw.channels.size())
      invalid("projected animation clone has undeclared channel suffixes");
  }
  std::copy(library_.restNodes.begin(),library_.restNodes.end(),prepared.library_.restNodes.begin());
  for(size_t n=0;n<oldNodes;++n) if(clone.nodes[n]>=0) {
    auto& rest=prepared.library_.restNodes[size_t(clone.nodes[n])];const auto& original=library_.restNodes[n];
    rest.matrix=original.matrix;rest.translation=original.translation;rest.rotation=original.rotation;rest.scale=original.scale;
    if(projected.retainMeshBindings[n]) rest.weights=original.weights;else rest.weights.clear();
  }
  prepared.library_.restMaterials=library_.restMaterials;prepared.library_.restLights=library_.restLights;
  prepared.library_.restCameras=library_.restCameras;
  prepared.selected_=selected_;prepared.time_=time_;prepared.speed_=speed_;prepared.playing_=playing_;prepared.loop_=loop_;
  return prepared;
}

namespace {
struct MaterialAnimationCopyBudget {
  const GltfMaterialAnimationEditLimits& limits;
  uint64_t bytes{}, visits{};
  explicit MaterialAnimationCopyBudget(const GltfMaterialAnimationEditLimits& value) : limits(value) {
    if (!limits.maxOwnedBytes || limits.maxOwnedBytes == UINT64_MAX || !limits.maxDecodedBytes ||
        limits.maxDecodedBytes == UINT64_MAX || !limits.maxMetadataValues || limits.maxMetadataValues == UINT64_MAX ||
        !limits.maxChannels || limits.maxChannels == UINT64_MAX || !limits.maxClips || limits.maxClips == UINT64_MAX ||
        !limits.maxMetadataDepth || limits.maxMetadataDepth > 256)
      invalid("material animation edit requires finite positive copy limits");
  }
  void add(uint64_t amount) {
    if (amount > limits.maxOwnedBytes - bytes) invalid("material animation edit copy budget exceeded");
    bytes += amount;
  }
  void string(std::string_view value) {
    add(sizeof(std::string));
    if (value.size() > (limits.maxOwnedBytes - bytes) / 2)
      invalid("material animation edit string budget exceeded");
    add(uint64_t(value.size()) * 2);
    add(1);
  }
  template<class T> void copy(const T& value, uint32_t depth = 0) {
    if (++visits > limits.maxMetadataValues || depth > limits.maxMetadataDepth)
      invalid("material animation edit metadata traversal budget exceeded");
    if constexpr (std::is_same_v<T, std::string>) string(value);
    else if constexpr (std::is_same_v<T, tinygltf::Value>) {
      // TinyGLTF copies inactive backing containers too. Reject malformed
      // resident tags before the active-value traversal can undercount them.
      if((!value.IsString() && !value.template Get<std::string>().empty()) ||
          (!value.IsBinary() && !value.template Get<std::vector<unsigned char>>().empty()) ||
          (!value.IsArray() && !value.template Get<tinygltf::Value::Array>().empty()) ||
          (!value.IsObject() && !value.template Get<tinygltf::Value::Object>().empty()) ||
          !(value.IsString() || value.IsBinary() || value.IsArray() || value.IsObject() ||
            value.IsBool() || value.IsNumber() || value.Type()==tinygltf::NULL_TYPE))
        invalid("material animation edit has invalid or inactive metadata storage");
      add(sizeof(T));
      if (value.IsString()) string(value.template Get<std::string>());
      else if (value.IsBinary()) add(value.template Get<std::vector<unsigned char>>().size());
      else if (value.IsNumber() && !std::isfinite(value.GetNumberAsDouble())) invalid("nonfinite material animation metadata");
      else if (value.IsArray()) for (const auto& item : value.template Get<tinygltf::Value::Array>()) copy(item, depth + 1);
      else if (value.IsObject()) for (const auto& [key, item] : value.template Get<tinygltf::Value::Object>()) {
        add(128); string(key); copy(item, depth + 1);
      }
    } else if constexpr (std::is_arithmetic_v<T> || std::is_enum_v<T>) add(sizeof(T));
    else if constexpr (requires { typename T::mapped_type; }) {
      add(sizeof(T));
      for (const auto& [key, item] : value) { add(128); copy(key, depth + 1); copy(item, depth + 1); }
    } else if constexpr (requires { typename T::value_type; value.begin(); }) {
      add(sizeof(T));
      if constexpr (std::is_arithmetic_v<typename T::value_type>) {
        const auto size = product(value.size(), sizeof(typename T::value_type)); add(size);
      } else for (const auto& item : value) copy(item, depth + 1);
    } else {
      static_assert(requires {value.extras;} || std::is_same_v<T, tinygltf::Parameter>);
      add(sizeof(T));
#define MATERIAL_ANIMATION_COPY(field) if constexpr (requires {value.field;}) copy(value.field, depth + 1)
      MATERIAL_ANIMATION_COPY(name); MATERIAL_ANIMATION_COPY(extras); MATERIAL_ANIMATION_COPY(extensions);
      MATERIAL_ANIMATION_COPY(extras_json_string); MATERIAL_ANIMATION_COPY(extensions_json_string);
      MATERIAL_ANIMATION_COPY(children); MATERIAL_ANIMATION_COPY(translation); MATERIAL_ANIMATION_COPY(rotation);
      MATERIAL_ANIMATION_COPY(scale); MATERIAL_ANIMATION_COPY(matrix); MATERIAL_ANIMATION_COPY(weights);
      MATERIAL_ANIMATION_COPY(pbrMetallicRoughness); MATERIAL_ANIMATION_COPY(baseColorFactor);
      MATERIAL_ANIMATION_COPY(baseColorTexture); MATERIAL_ANIMATION_COPY(metallicRoughnessTexture);
      MATERIAL_ANIMATION_COPY(normalTexture); MATERIAL_ANIMATION_COPY(occlusionTexture); MATERIAL_ANIMATION_COPY(emissiveTexture);
      MATERIAL_ANIMATION_COPY(emissiveFactor); MATERIAL_ANIMATION_COPY(alphaMode); MATERIAL_ANIMATION_COPY(lods);
      MATERIAL_ANIMATION_COPY(values); MATERIAL_ANIMATION_COPY(additionalValues); MATERIAL_ANIMATION_COPY(string_value);
      MATERIAL_ANIMATION_COPY(number_array); MATERIAL_ANIMATION_COPY(json_double_value);
      MATERIAL_ANIMATION_COPY(color); MATERIAL_ANIMATION_COPY(type); MATERIAL_ANIMATION_COPY(spot);
      MATERIAL_ANIMATION_COPY(perspective); MATERIAL_ANIMATION_COPY(orthographic);
      MATERIAL_ANIMATION_COPY(channels); MATERIAL_ANIMATION_COPY(samplers); MATERIAL_ANIMATION_COPY(interpolation);
      MATERIAL_ANIMATION_COPY(target_path); MATERIAL_ANIMATION_COPY(target_extensions); MATERIAL_ANIMATION_COPY(target_extras);
      MATERIAL_ANIMATION_COPY(target_extensions_json_string); MATERIAL_ANIMATION_COPY(target_extras_json_string);
      MATERIAL_ANIMATION_COPY(minValues); MATERIAL_ANIMATION_COPY(maxValues); MATERIAL_ANIMATION_COPY(sparse);
      MATERIAL_ANIMATION_COPY(indices); MATERIAL_ANIMATION_COPY(uri);
#undef MATERIAL_ANIMATION_COPY
    }
  }
  void library(const AnimationLibrary& value) {
    add(sizeof(AnimationLibrary));
    for (const auto& clip : value.clips) {
      add(sizeof(AnimationClip)); string(clip.name);
      for (const auto& track : clip.tracks) {
        add(sizeof(AnimationTrack)); string(track.target.property); copy(track.times); copy(track.values);
      }
    }
    for (const auto& diagnostic : value.diagnostics) { add(sizeof(AnimationDiagnostic)); string(diagnostic.message); }
    copy(value.restNodes); copy(value.restMaterials); copy(value.restLights); copy(value.restCameras);
  }
};
void materialAnimationCompatibility(const tinygltf::Model& current, const AnimationLibrary& retained,
    MaterialAnimationCopyBudget& budget) {
  if (current.animations.size() != retained.clips.size() || current.animations.size() > budget.limits.maxClips)
    invalid("material edit changed animation clip identities");
  uint64_t channels{};
  for (const auto& clip : current.animations) {
    if (clip.channels.size() > budget.limits.maxChannels - channels)
      invalid("material edit animation channel budget exceeded");
    channels += clip.channels.size();
    budget.copy(clip);
    // Parsed target strings/container growth and diagnostics are bounded before
    // decoding; source binary/image payloads never enter this copy accounting.
    if (clip.channels.size() > (budget.limits.maxOwnedBytes - budget.bytes) / 512)
      invalid("material edit parsed channel storage exceeds its budget");
    budget.add(uint64_t(clip.channels.size()) * 512);
  }
  const auto available = std::min(budget.limits.maxDecodedBytes, budget.limits.maxOwnedBytes - budget.bytes);
  // No quadratic overlap scan is needed here: exact matching to the already
  // validated retained tracks proves the same target set without rechecking it.
  const auto decoded = parseAnimations(current, false, size_t(available), false);
  if (decoded.clips.size() != retained.clips.size() || decoded.diagnostics.size() != retained.diagnostics.size())
    invalid("material edit changed animation channels or diagnostics");
  for (size_t i = 0; i < retained.clips.size(); ++i) {
    const auto& a = retained.clips[i]; const auto& b = decoded.clips[i];
    if (a.name != b.name || std::bit_cast<uint64_t>(a.duration) != std::bit_cast<uint64_t>(b.duration) || a.tracks.size() != b.tracks.size())
      invalid("material edit changed a decoded animation clip");
    for (size_t track = 0; track < a.tracks.size(); ++track)
      if (!cloneTrackEqual(a.tracks[track], b.tracks[track], a.tracks[track].target))
        invalid("material edit changed an animation target or curve");
  }
  for (size_t i = 0; i < retained.diagnostics.size(); ++i) {
    const auto& a = retained.diagnostics[i]; const auto& b = decoded.diagnostics[i];
    if (a.clip != b.clip || a.channel != b.channel || a.message != b.message)
      invalid("material edit changed an animation diagnostic");
  }
}
void materialTailRawTargets(const tinygltf::Model& current, size_t retainedMaterials,
    MaterialAnimationCopyBudget& budget, std::optional<size_t> forbiddenMaterial = {}) {
  if(current.animations.size()>budget.limits.maxClips) invalid("material tail clip budget exceeded");
  uint64_t channels{};
  for(const auto& clip:current.animations) {
    if(clip.channels.size()>budget.limits.maxChannels-channels)
      invalid("material tail raw channel budget exceeded");
    channels+=clip.channels.size();
    // Bound raw metadata before reparsing or copying the player. The later
    // compatibility check conservatively charges this traversal again.
    budget.copy(clip);
    for(const auto& channel:clip.channels) {
      const auto extension=channel.target_extensions.find("KHR_animation_pointer");
      if(extension==channel.target_extensions.end()) continue;
      if(!extension->second.IsObject() || !extension->second.Has("pointer") ||
          !extension->second.Get("pointer").IsString()) invalid("material tail has a malformed raw animation pointer");
      const auto& pointer=extension->second.Get("pointer").Get<std::string>();
      // splitPointer used by the decoder owns a string per token. A string of
      // empty slash-separated tokens must not evade its temporary storage cap.
      budget.add(product(pointer.size(),4*sizeof(std::string)+8));
      if(pointer.empty() || pointer.front()!='/') invalid("material tail has an invalid raw animation pointer");
      for(size_t i=0;i<pointer.size();++i) if(pointer[i]=='~') {
        if(++i==pointer.size() || (pointer[i]!='0' && pointer[i]!='1'))
          invalid("material tail has an invalid raw animation pointer escape");
      }
      const std::string_view path(pointer);
      const auto rootEnd=path.find('/',1);
      if(path.substr(1,rootEnd==std::string_view::npos?rootEnd:rootEnd-1)!="materials") continue;
      if(rootEnd==std::string_view::npos) invalid("material tail is referenced by a whole material-table pointer");
      const auto indexEnd=path.find('/',rootEnd+1);
      const auto token=path.substr(rootEnd+1,indexEnd==std::string_view::npos?indexEnd:indexEnd-rootEnd-1);
      size_t index{};
      const auto parsed=std::from_chars(token.data(),token.data()+token.size(),index);
      if(token.empty() || (token.size()>1 && token.front()=='0') || parsed.ec!=std::errc{} ||
          parsed.ptr!=token.data()+token.size() || index>=retainedMaterials || index==forbiddenMaterial)
        invalid("material tail is referenced by a removed or unavailable raw material target");
    }
  }
}
// Restore a supported target, not the material snapshot. Authored and evaluated
// records can have different texture index spaces and untracked frozen values.
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
    if(!caches[index] || projectedExact(out.extensions,before.extensions)) return;
    if(authored && projectedExact(out.extensions,authored->extensions)) out.extensions_json_string=authored->extensions_json_string;
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

AnimationPlayer AnimationPlayer::withMaterialEdits(const tinygltf::Model& current,
    const GltfMaterialAuthoringPlan& plan, const GltfMaterialAnimationEditLimits& limits) const {
  // The context accessor checks the opaque receipt and all public fields before
  // any caller-controlled record can influence copying, indexing or sampling.
  const auto context = GltfMaterialAuthoringContext(plan);
  const GltfMaterialAuthoringSourceView authored{context.materialCount, context.authoredTextureCount,
      context.authoredDeclarations, current.animations};
  if(context.resourceTransition) {
    if(!plan.sourceMaterial || size_t(*plan.sourceMaterial)>=current.materials.size() ||
        size_t(*plan.sourceMaterial)>=library_.restMaterials.size())
      invalid("texture material edit requires a current existing rest");
    const GltfMaterialAuthoringSourceView afterAuthored{context.materialCount,context.afterAuthoredTextureCount,
        context.afterAuthoredDeclarations,current.animations};
    const GltfMaterialAuthoringSourceView afterEvaluated{context.materialCount,context.afterEvaluatedTextureCount,
        context.afterEvaluatedDeclarations,current.animations};
    const auto source=size_t(*plan.sourceMaterial);
    ValidateGltfMaterialEditTransition(plan,library_.restMaterials[source],current.materials[source],
        authored,GltfMaterialSourceView(current),afterAuthored,afterEvaluated,limits.authoring);
  } else ValidateGltfMaterialAuthoringPlan(plan, authored, GltfMaterialSourceView(current), limits.authoring);
  MaterialAnimationCopyBudget budget(limits);
  if (current.materials.size() != plan.originalMaterialCount || library_.restMaterials.size() != plan.originalMaterialCount ||
      current.nodes.size() != library_.restNodes.size() || current.lights.size() != library_.restLights.size() ||
      current.cameras.size() != library_.restCameras.size()) invalid("material edit changed source/rest table identities");
  if (plan.sourceMaterial) {
    const auto index = size_t(*plan.sourceMaterial);
    if (index >= current.materials.size() || !projectedExact(current.materials[index], plan.evaluatedBefore) ||
        !projectedExact(library_.restMaterials[index], plan.authoredBefore))
      invalid("material edit has stale evaluated or authored before data");
  }
  budget.library(library_); budget.library(library_);
  budget.copy(plan.authoredAfter); budget.copy(plan.authoredAfter);
  if(plan.kind!=GltfMaterialPlanKind::Edit) materialTailRawTargets(current,current.materials.size(),budget);
  materialAnimationCompatibility(current, library_, budget);
  auto prepared = *this;
  if (plan.kind == GltfMaterialPlanKind::Edit)
    prepared.library_.restMaterials[size_t(plan.destinationMaterial)] = plan.authoredAfter;
  else prepared.library_.restMaterials.push_back(plan.authoredAfter);
  return prepared;
}

AnimationPlayer AnimationPlayer::withoutLastMaterialRest(const tinygltf::Model& current,
    const tinygltf::Material& expectedRest, const GltfMaterialAnimationEditLimits& limits) const {
  MaterialAnimationCopyBudget budget(limits);
  if(library_.restMaterials.empty() || current.materials.size()!=library_.restMaterials.size() ||
      current.nodes.size()!=library_.restNodes.size() || current.lights.size()!=library_.restLights.size() ||
      current.cameras.size()!=library_.restCameras.size() || current.animations.size()!=library_.clips.size())
    invalid("material tail removal changed source/rest table identities");
  if(library_.clips.size()>limits.maxClips || (selected_ && *selected_>=library_.clips.size()) ||
      !std::isfinite(time_) || time_<0 || !std::isfinite(speed_)) invalid("material tail removal has invalid playback state");
  const auto retainedMaterials=library_.restMaterials.size()-1;
  uint64_t channels{};
  for(const auto& clip:library_.clips) {
    if(clip.tracks.size()>limits.maxChannels-channels) invalid("material tail decoded channel budget exceeded");
    channels+=clip.tracks.size();
    for(const auto& track:clip.tracks)
      if(track.target.kind==AnimationTargetKind::Material && track.target.index>=retainedMaterials)
        invalid("material tail is referenced by a removed or unavailable decoded target");
  }
  budget.add(GltfMaterialRecordStorageBytes(expectedRest,limits.authoring));
  budget.add(GltfMaterialRecordStorageBytes(library_.restMaterials.back(),limits.authoring));
  if(!GltfMaterialRecordsEqual(expectedRest,library_.restMaterials.back()))
    invalid("material tail authored rest witness is stale");
  // Charge the complete retained library and detached output before decoding
  // scratch or copying any rest/clip. No image, geometry or Model copy occurs.
  budget.library(library_);budget.library(library_);
  materialTailRawTargets(current,retainedMaterials,budget);
  materialAnimationCompatibility(current,library_,budget);
  auto prepared=*this;
  prepared.library_.restMaterials.pop_back();
  return prepared;
}

AnimationPlayer AnimationPlayer::withUntrackedMaterialRest(const tinygltf::Model& current,
    GltfMaterialAuthoringSourceView authored, size_t material,
    const tinygltf::Material& expectedRest, const tinygltf::Material& replacement,
    const GltfMaterialAnimationEditLimits& limits) const {
  MaterialAnimationCopyBudget budget(limits);
  if(material>=library_.restMaterials.size() || current.materials.size()!=library_.restMaterials.size() ||
      authored.materialCount!=library_.restMaterials.size() || current.nodes.size()!=library_.restNodes.size() ||
      current.lights.size()!=library_.restLights.size() || current.cameras.size()!=library_.restCameras.size() ||
      current.animations.size()!=library_.clips.size())
    invalid("untracked material replacement changed source/rest identities");
  if(library_.clips.size()>limits.maxClips || (selected_ && *selected_>=library_.clips.size()) ||
      !std::isfinite(time_) || time_<0 || !std::isfinite(speed_))
    invalid("untracked material replacement has invalid playback state");
  budget.add(GltfMaterialRecordStorageBytes(expectedRest,limits.authoring));
  budget.add(GltfMaterialRecordStorageBytes(library_.restMaterials[material],limits.authoring));
  if(!GltfMaterialRecordsEqual(expectedRest,library_.restMaterials[material]))
    invalid("untracked material authored rest witness is stale");
  uint64_t channels{};
  for(const auto& clip:library_.clips) {
    if(clip.tracks.size()>limits.maxChannels-channels) invalid("untracked material decoded channel budget exceeded");
    channels+=clip.tracks.size();
    for(const auto& track:clip.tracks) if(track.target.kind==AnimationTargetKind::Material &&
        (track.target.index>=library_.restMaterials.size() || track.target.index==material))
      invalid("untracked material is referenced by a decoded animation target");
  }
  budget.library(library_);budget.library(library_);
  budget.copy(replacement);budget.copy(replacement);
  budget.add(product(GltfMaterialRecordStorageBytes(replacement,limits.authoring),6));
  if(authored.extensionsUsed.size()>limits.maxMetadataValues)
    invalid("untracked material declaration budget exceeded");
  for(const auto& name:authored.extensionsUsed) budget.string(name);
  auto validation=limits.authoring;
  validation.maxOutputBytes=std::min(validation.maxOutputBytes,limits.maxOwnedBytes);
  ValidateGltfMaterialRecord(replacement,authored,validation);
  materialTailRawTargets(current,current.materials.size(),budget,material);
  materialAnimationCompatibility(current,library_,budget);
  auto prepared=*this;
  prepared.library_.restMaterials[material]=replacement;
  return prepared;
}

void AnimationPlayer::validateMaterialEditCurves(const tinygltf::Model& current,
    const GltfMaterialAnimationEditLimits& limits) const {
  MaterialAnimationCopyBudget budget(limits);budget.library(library_);
  materialAnimationCompatibility(current,library_,budget);
}

namespace {
enum class CurveOwnerKind : uint8_t { Node, Material, Camera, Light };
struct CurveOwnerRef {
  CurveOwnerKind kind{}; size_t index{};
  bool operator==(const CurveOwnerRef&) const = default;
  bool operator<(const CurveOwnerRef& other) const noexcept {
    return kind != other.kind ? kind < other.kind : index < other.index;
  }
};
template<class T> struct CurveSourceRow { size_t index{}; T record; };
struct CurveMeshShape {
  size_t index{}, defaultWeights{};
  std::vector<size_t> primitiveTargets;
};
struct CurveEncodedRange {
  size_t buffer{}, offset{}, stride{}, count{}, width{};
  std::vector<unsigned char> bytes;
};
struct CurveBufferMetadata {
  size_t index{}, dataSize{};
  // data stays empty: a shared geometry/animation buffer is never copied.
  tinygltf::Buffer record;
};
struct CurveExactBudget {
  uint64_t values{};
  uint32_t maxDepth{};
};
bool curveValueStorage(const tinygltf::Value& v) noexcept {
  using V = tinygltf::Value;
  return (v.IsString() || v.Get<std::string>().empty()) &&
      (v.IsBinary() || v.Get<std::vector<unsigned char>>().empty()) &&
      (v.IsArray() || v.Get<V::Array>().empty()) && (v.IsObject() || v.Get<V::Object>().empty()) &&
      (v.IsString() || v.IsBinary() || v.IsArray() || v.IsObject() ||
       v.IsBool() || v.IsNumber() || v.Type() == tinygltf::NULL_TYPE);
}
// Separate from projectedExact/cloneRawValueEqual: those clone helpers use an
// allocating work stack. This recursion has a captured hard depth/value bound.
template<class T> bool curveExact(const T& a, const T& b, CurveExactBudget& budget, uint32_t depth = 0) noexcept {
  using V = tinygltf::Value;
  if constexpr (std::is_same_v<T, V>) {
    if (!budget.values || depth > budget.maxDepth || a.Type() != b.Type() ||
        !curveValueStorage(a) || !curveValueStorage(b)) return false;
    --budget.values;
    if (a.IsInt()) return a.template Get<int>() == b.template Get<int>();
    if (a.IsReal()) return std::bit_cast<uint64_t>(a.template Get<double>()) == std::bit_cast<uint64_t>(b.template Get<double>());
    if (a.IsBool()) return a.template Get<bool>() == b.template Get<bool>();
    if (a.IsString()) return a.template Get<std::string>() == b.template Get<std::string>();
    if (a.IsBinary()) return a.template Get<std::vector<unsigned char>>() == b.template Get<std::vector<unsigned char>>();
    if (a.IsArray()) {
      const auto& x = a.template Get<V::Array>(); const auto& y = b.template Get<V::Array>();
      if (x.size() != y.size()) return false;
      for (size_t i = 0; i < x.size(); ++i) if (!curveExact(x[i], y[i], budget, depth + 1)) return false;
    } else if (a.IsObject()) {
      const auto& x = a.template Get<V::Object>(); const auto& y = b.template Get<V::Object>();
      if (x.size() != y.size()) return false;
      auto next = y.begin();
      for (const auto& [key, value] : x) {
        if (key != next->first || !curveExact(value, next->second, budget, depth + 1)) return false;
        ++next;
      }
    }
    return true;
  } else if constexpr (std::is_same_v<T, double>) return std::bit_cast<uint64_t>(a) == std::bit_cast<uint64_t>(b);
  else if constexpr (std::is_arithmetic_v<T> || std::is_enum_v<T> || std::is_same_v<T, std::string>) return a == b;
  else if constexpr (requires { typename T::mapped_type; }) {
    if (a.size() != b.size()) return false;
    auto next = b.begin();
    for (const auto& [key, value] : a) {
      if (key != next->first || !curveExact(value, next->second, budget)) return false;
      ++next;
    }
    return true;
  } else if constexpr (requires { typename T::value_type; a.begin(); }) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) if (!curveExact(a[i], b[i], budget)) return false;
    return true;
  } else if constexpr (requires { a.isSparse; }) {
    if (a.isSparse != b.isSparse || !curveExact(a.extras, b.extras, budget) ||
        !curveExact(a.extensions, b.extensions, budget) || a.extras_json_string != b.extras_json_string ||
        a.extensions_json_string != b.extensions_json_string) return false;
    const auto metadata = [&](const auto& x, const auto& y) {
      return curveExact(x.extras, y.extras, budget) && curveExact(x.extensions, y.extensions, budget) &&
          x.extras_json_string == y.extras_json_string && x.extensions_json_string == y.extensions_json_string;
    };
    if (!metadata(a.indices, b.indices) || !metadata(a.values, b.values)) return false;
    // TinyGLTF's inactive sparse numeric fields are indeterminate.
    return !a.isSparse || (a.count == b.count && a.indices.byteOffset == b.indices.byteOffset &&
        a.indices.bufferView == b.indices.bufferView && a.indices.componentType == b.indices.componentType &&
        a.values.byteOffset == b.values.byteOffset && a.values.bufferView == b.values.bufferView);
  } else {
    static_assert(requires { a.extras; } || std::is_same_v<T, tinygltf::Parameter>);
#define CURVE_EXACT_FIELD(field) if constexpr (requires { a.field; }) if (!curveExact(a.field, b.field, budget)) return false
    CURVE_EXACT_FIELD(name); CURVE_EXACT_FIELD(extras); CURVE_EXACT_FIELD(extensions);
    CURVE_EXACT_FIELD(extras_json_string); CURVE_EXACT_FIELD(extensions_json_string);
    CURVE_EXACT_FIELD(channels); CURVE_EXACT_FIELD(samplers); CURVE_EXACT_FIELD(input); CURVE_EXACT_FIELD(output);
    CURVE_EXACT_FIELD(interpolation); CURVE_EXACT_FIELD(sampler); CURVE_EXACT_FIELD(target_node);
    CURVE_EXACT_FIELD(target_path); CURVE_EXACT_FIELD(target_extensions); CURVE_EXACT_FIELD(target_extras);
    CURVE_EXACT_FIELD(target_extensions_json_string); CURVE_EXACT_FIELD(target_extras_json_string);
    CURVE_EXACT_FIELD(buffer); CURVE_EXACT_FIELD(bufferView); CURVE_EXACT_FIELD(byteOffset);
    CURVE_EXACT_FIELD(byteLength); CURVE_EXACT_FIELD(byteStride); CURVE_EXACT_FIELD(target); CURVE_EXACT_FIELD(dracoDecoded);
    CURVE_EXACT_FIELD(normalized); CURVE_EXACT_FIELD(componentType); CURVE_EXACT_FIELD(count); CURVE_EXACT_FIELD(type);
    CURVE_EXACT_FIELD(minValues); CURVE_EXACT_FIELD(maxValues); CURVE_EXACT_FIELD(sparse); CURVE_EXACT_FIELD(uri);
    CURVE_EXACT_FIELD(camera); CURVE_EXACT_FIELD(mesh); CURVE_EXACT_FIELD(skin); CURVE_EXACT_FIELD(light);
    CURVE_EXACT_FIELD(emitter); CURVE_EXACT_FIELD(children); CURVE_EXACT_FIELD(translation); CURVE_EXACT_FIELD(rotation);
    CURVE_EXACT_FIELD(scale); CURVE_EXACT_FIELD(matrix); CURVE_EXACT_FIELD(weights); CURVE_EXACT_FIELD(lods);
    CURVE_EXACT_FIELD(pbrMetallicRoughness); CURVE_EXACT_FIELD(baseColorFactor); CURVE_EXACT_FIELD(baseColorTexture);
    CURVE_EXACT_FIELD(metallicFactor); CURVE_EXACT_FIELD(roughnessFactor); CURVE_EXACT_FIELD(metallicRoughnessTexture);
    CURVE_EXACT_FIELD(normalTexture); CURVE_EXACT_FIELD(occlusionTexture); CURVE_EXACT_FIELD(emissiveTexture);
    CURVE_EXACT_FIELD(emissiveFactor); CURVE_EXACT_FIELD(alphaMode); CURVE_EXACT_FIELD(alphaCutoff); CURVE_EXACT_FIELD(doubleSided);
    CURVE_EXACT_FIELD(values); CURVE_EXACT_FIELD(additionalValues); CURVE_EXACT_FIELD(index); CURVE_EXACT_FIELD(texCoord);
    CURVE_EXACT_FIELD(strength); CURVE_EXACT_FIELD(bool_value); CURVE_EXACT_FIELD(has_number_value);
    CURVE_EXACT_FIELD(string_value); CURVE_EXACT_FIELD(number_array); CURVE_EXACT_FIELD(json_double_value); CURVE_EXACT_FIELD(number_value);
    CURVE_EXACT_FIELD(perspective); CURVE_EXACT_FIELD(orthographic); CURVE_EXACT_FIELD(aspectRatio);
    CURVE_EXACT_FIELD(yfov); CURVE_EXACT_FIELD(znear); CURVE_EXACT_FIELD(zfar); CURVE_EXACT_FIELD(xmag); CURVE_EXACT_FIELD(ymag);
    CURVE_EXACT_FIELD(color); CURVE_EXACT_FIELD(intensity); CURVE_EXACT_FIELD(range); CURVE_EXACT_FIELD(spot);
    CURVE_EXACT_FIELD(innerConeAngle); CURVE_EXACT_FIELD(outerConeAngle);
#undef CURVE_EXACT_FIELD
    return true;
  }
}
void curveLibraryBudget(const AnimationLibrary& library, MaterialAnimationCopyBudget& budget) {
  budget.add(product(sizeof(AnimationClip), library.clips.size()));
  for (const auto& clip : library.clips) {
    budget.string(clip.name);
    for (const auto& track : clip.tracks) {
      budget.add(sizeof(AnimationTrack)); budget.string(track.target.property); budget.copy(track.times); budget.copy(track.values);
    }
  }
  for (const auto& diagnostic : library.diagnostics) {
    budget.add(sizeof(AnimationDiagnostic)); budget.string(diagnostic.message);
  }
}
bool curveLibraryMatches(const std::vector<AnimationClip>& clips, const std::vector<AnimationDiagnostic>& diagnostics,
    const AnimationLibrary& current) noexcept {
  if (clips.size() != current.clips.size() || diagnostics.size() != current.diagnostics.size()) return false;
  for (size_t i = 0; i < clips.size(); ++i) {
    const auto& a = clips[i]; const auto& b = current.clips[i];
    if (a.name != b.name || std::bit_cast<uint64_t>(a.duration) != std::bit_cast<uint64_t>(b.duration) || a.tracks.size() != b.tracks.size()) return false;
    for (size_t t = 0; t < a.tracks.size(); ++t) if (!cloneTrackEqual(a.tracks[t], b.tracks[t], a.tracks[t].target)) return false;
  }
  for (size_t i = 0; i < diagnostics.size(); ++i)
    if (diagnostics[i].clip != current.diagnostics[i].clip || diagnostics[i].channel != current.diagnostics[i].channel ||
        diagnostics[i].message != current.diagnostics[i].message) return false;
  return true;
}
std::optional<CurveOwnerRef> curveSourceOwner(const tinygltf::AnimationChannel& channel) noexcept {
  if (channel.target_path != "pointer") {
    if (channel.target_node >= 0) return CurveOwnerRef{CurveOwnerKind::Node, size_t(channel.target_node)};
    return {};
  }
  const auto ext = std::find_if(channel.target_extensions.begin(), channel.target_extensions.end(),
      [](const auto& item) { return item.first == "KHR_animation_pointer"; });
  if (ext == channel.target_extensions.end() || !ext->second.IsObject()) return {};
  const auto& pointer = ext->second.Get("pointer");
  if (!pointer.IsString()) return {};
  std::string_view path = pointer.Get<std::string>(); CurveOwnerKind kind;
  if (path.starts_with("/nodes/")) { kind = CurveOwnerKind::Node; path.remove_prefix(7); }
  else if (path.starts_with("/materials/")) { kind = CurveOwnerKind::Material; path.remove_prefix(11); }
  else if (path.starts_with("/cameras/")) { kind = CurveOwnerKind::Camera; path.remove_prefix(9); }
  else if (path.starts_with("/extensions/KHR_lights_punctual/lights/")) {
    kind = CurveOwnerKind::Light; path.remove_prefix(std::string_view("/extensions/KHR_lights_punctual/lights/").size());
  }
  else return {};
  const auto end = path.find('/'); const auto token = path.substr(0, end); size_t index{};
  if (token.empty() || (token.size() > 1 && token.front() == '0')) return {};
  for (const char ch : token) {
    if (ch < '0' || ch > '9' || index > (SIZE_MAX - size_t(ch - '0')) / 10) return {};
    index = index * 10 + size_t(ch - '0');
  }
  return CurveOwnerRef{kind, index};
}
template<class T> void curveUnique(std::vector<T>& values) {
  std::sort(values.begin(), values.end()); values.erase(std::unique(values.begin(), values.end()), values.end());
}
template<class T> void curveCopyMetadata(T& to, const T& from) {
  to.extras = from.extras; to.extensions = from.extensions;
  to.extras_json_string = from.extras_json_string; to.extensions_json_string = from.extensions_json_string;
}
void curveCopyAccessor(tinygltf::Accessor& to, const tinygltf::Accessor& from) {
  to.sparse = {};
  to.bufferView = from.bufferView; to.name = from.name; to.byteOffset = from.byteOffset; to.normalized = from.normalized;
  to.componentType = from.componentType; to.count = from.count; to.type = from.type;
  to.minValues = from.minValues; to.maxValues = from.maxValues; curveCopyMetadata(to, from);
  to.sparse.isSparse = from.sparse.isSparse; curveCopyMetadata(to.sparse, from.sparse);
  curveCopyMetadata(to.sparse.indices, from.sparse.indices); curveCopyMetadata(to.sparse.values, from.sparse.values);
  if (from.sparse.isSparse) {
    to.sparse.count = from.sparse.count; to.sparse.indices.byteOffset = from.sparse.indices.byteOffset;
    to.sparse.indices.bufferView = from.sparse.indices.bufferView; to.sparse.indices.componentType = from.sparse.indices.componentType;
    to.sparse.values.byteOffset = from.sparse.values.byteOffset; to.sparse.values.bufferView = from.sparse.values.bufferView;
  }
}
size_t curveComponentWidth(int type) {
  switch (type) {
    case TINYGLTF_COMPONENT_TYPE_BYTE: case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE: return 1;
    case TINYGLTF_COMPONENT_TYPE_SHORT: case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT: return 2;
    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT: case TINYGLTF_COMPONENT_TYPE_FLOAT: return 4;
    case TINYGLTF_COMPONENT_TYPE_DOUBLE: return 8;
    default: invalid("material curve witness has an unsupported encoded component");
  }
}
// Existing CheckedAccessor proves these layouts during strict preparation.
// Gathering their physical ranges is byte copying, not another value decoder.
template<class Visit> void curveAccessorRanges(const tinygltf::Model& model, const tinygltf::Accessor& accessor, Visit&& visit) {
  const size_t width = product(vectorComponents(accessor.type), curveComponentWidth(accessor.componentType));
  if (accessor.bufferView >= 0) {
    const auto& view = checked(model.bufferViews, accessor.bufferView, "curve witness view");
    visit(accessor.bufferView, accessor.byteOffset, accessor.count, width, view.byteStride ? view.byteStride : width);
  }
  if (accessor.sparse.isSparse) {
    const size_t count = size_t(accessor.sparse.count), indexWidth = curveComponentWidth(accessor.sparse.indices.componentType);
    visit(accessor.sparse.indices.bufferView, accessor.sparse.indices.byteOffset, count, indexWidth, indexWidth);
    visit(accessor.sparse.values.bufferView, accessor.sparse.values.byteOffset, count, width, width);
  }
}
} // namespace

struct MaterialAnimationCurveWitness::State {
  GltfMaterialAnimationEditLimits limits;
  uint64_t bytes{};
  size_t nodes{}, materials{}, cameras{}, lights{}, meshes{}, accessors{}, views{}, buffers{};
  std::vector<std::string> requiredExtensions;
  std::vector<tinygltf::Animation> raw;
  std::vector<AnimationClip> clips;
  std::vector<AnimationDiagnostic> diagnostics;
  std::vector<CurveSourceRow<tinygltf::Node>> nodeRows;
  std::vector<CurveSourceRow<tinygltf::Material>> materialRows;
  std::vector<CurveSourceRow<tinygltf::Camera>> cameraRows;
  std::vector<CurveSourceRow<tinygltf::Light>> lightRows;
  std::vector<CurveMeshShape> morphRows;
  std::vector<CurveSourceRow<tinygltf::Accessor>> accessorRows;
  std::vector<CurveSourceRow<tinygltf::BufferView>> viewRows;
  std::vector<CurveBufferMetadata> bufferRows;
  std::vector<CurveEncodedRange> ranges;
};

MaterialAnimationCurveWitness::operator bool() const noexcept { return bool(state_); }
uint64_t MaterialAnimationCurveWitness::ownedBytes() const noexcept { return state_ ? state_->bytes : 0; }
bool MaterialAnimationCurveWitness::matches(const tinygltf::Model& current, const AnimationPlayer& player) const noexcept {
  if (!state_) return false;
  const auto& s = *state_;
  if (current.nodes.size() != s.nodes || current.materials.size() != s.materials || current.cameras.size() != s.cameras ||
      current.lights.size() != s.lights || current.meshes.size() != s.meshes || current.accessors.size() != s.accessors ||
      current.bufferViews.size() != s.views || current.buffers.size() != s.buffers || current.extensionsRequired != s.requiredExtensions ||
      !curveLibraryMatches(s.clips, s.diagnostics, player.library())) return false;
  CurveExactBudget budget{s.limits.maxMetadataValues, s.limits.maxMetadataDepth};
  if (!curveExact(current.animations, s.raw, budget)) return false;
  const auto rows = [&](const auto& source, const auto& retained) {
    for (const auto& row : retained)
      if (row.index >= source.size() || !curveExact(source[row.index], row.record, budget)) return false;
    return true;
  };
  if (!rows(current.nodes, s.nodeRows) || !rows(current.materials, s.materialRows) || !rows(current.cameras, s.cameraRows) ||
      !rows(current.lights, s.lightRows) || !rows(current.accessors, s.accessorRows) || !rows(current.bufferViews, s.viewRows)) return false;
  for (const auto& row : s.bufferRows)
    if (row.index >= current.buffers.size() || current.buffers[row.index].data.size() != row.dataSize ||
        !curveExact(current.buffers[row.index], row.record, budget)) return false;
  for (const auto& row : s.morphRows) {
    if (row.index >= current.meshes.size()) return false;
    const auto& mesh = current.meshes[row.index];
    if (mesh.weights.size() != row.defaultWeights || mesh.primitives.size() != row.primitiveTargets.size()) return false;
    for (size_t p = 0; p < mesh.primitives.size(); ++p)
      if (mesh.primitives[p].targets.size() != row.primitiveTargets[p]) return false;
  }
  for (const auto& range : s.ranges) {
    if (range.buffer >= current.buffers.size()) return false;
    const auto& data = current.buffers[range.buffer].data;
    if (range.offset > data.size() || !range.count || range.width > data.size() - range.offset ||
        range.count - 1 > (data.size() - range.offset - range.width) / range.stride ||
        range.bytes.size() != range.count * range.width) return false;
    for (size_t i = 0; i < range.count; ++i)
      if (std::memcmp(data.data() + range.offset + i * range.stride, range.bytes.data() + i * range.width, range.width) != 0) return false;
  }
  return true;
}

MaterialAnimationCurveWitness AnimationPlayer::prepareMaterialEditCurveWitness(const tinygltf::Model& current,
    const GltfMaterialAnimationEditLimits& limits) const {
  validateMaterialEditCurves(current, limits);
  MaterialAnimationCopyBudget budget(limits);
  budget.add(sizeof(MaterialAnimationCurveWitness::State) + 256);
  budget.copy(current.animations); budget.copy(current.extensionsRequired); curveLibraryBudget(library_, budget);
  uint64_t channels{};
  for (const auto& clip : current.animations) channels += clip.channels.size(); // strict validation bounded this sum
  budget.add(product(size_t(channels), sizeof(CurveOwnerRef) + 2 * sizeof(int)));
  std::vector<CurveOwnerRef> owners; owners.reserve(size_t(channels));
  std::vector<int> accessorIds; accessorIds.reserve(product(size_t(channels), 2));
  size_t diagnostic{};
  for (size_t c = 0; c < current.animations.size(); ++c) for (size_t k = 0; k < current.animations[c].channels.size(); ++k) {
    const auto& channel = current.animations[c].channels[k];
    if (const auto owner = curveSourceOwner(channel)) {
      const size_t count = owner->kind == CurveOwnerKind::Node ? current.nodes.size() :
          owner->kind == CurveOwnerKind::Material ? current.materials.size() :
          owner->kind == CurveOwnerKind::Camera ? current.cameras.size() : current.lights.size();
      if (owner->index < count) owners.push_back(*owner);
    }
    const bool omitted = diagnostic < library_.diagnostics.size() && library_.diagnostics[diagnostic].clip == c &&
        library_.diagnostics[diagnostic].channel == k;
    if (omitted) { ++diagnostic; continue; }
    const auto& sampler = checked(current.animations[c].samplers, channel.sampler, "curve witness sampler");
    accessorIds.push_back(sampler.input); accessorIds.push_back(sampler.output);
  }
  curveUnique(owners); curveUnique(accessorIds);
  std::vector<int> viewIds; std::vector<size_t> meshIds;
  budget.add(product(accessorIds.size(), 3 * sizeof(int))); viewIds.reserve(product(accessorIds.size(), 3));
  budget.add(product(owners.size(), sizeof(size_t))); meshIds.reserve(owners.size());
  std::array<size_t, 4> ownerCounts{};
  for (const auto& owner : owners) {
    ++ownerCounts[size_t(owner.kind)];
    budget.add(sizeof(size_t) + 128);
    if (owner.kind == CurveOwnerKind::Node) {
      const auto& node = current.nodes[owner.index]; budget.copy(node);
      if (node.mesh >= 0 && size_t(node.mesh) < current.meshes.size()) meshIds.push_back(size_t(node.mesh));
    } else if (owner.kind == CurveOwnerKind::Material) budget.copy(current.materials[owner.index]);
    else if (owner.kind == CurveOwnerKind::Camera) budget.copy(current.cameras[owner.index]);
    else budget.copy(current.lights[owner.index]);
  }
  curveUnique(meshIds);
  for (const auto mesh : meshIds) {
    budget.add(sizeof(CurveMeshShape)); budget.add(product(current.meshes[mesh].primitives.size(), sizeof(size_t)));
  }
  size_t rangeCount{};
  for (const int id : accessorIds) {
    const auto& accessor = checked(current.accessors, id, "curve witness accessor");
    budget.add(sizeof(size_t) + 128); budget.copy(accessor);
    curveAccessorRanges(current, accessor, [&](int view, size_t, size_t count, size_t width, size_t) {
      viewIds.push_back(view); ++rangeCount;
      budget.add(sizeof(CurveEncodedRange) + 64); budget.add(product(count, width));
    });
  }
  curveUnique(viewIds);
  std::vector<int> bufferIds; budget.add(product(viewIds.size(), sizeof(int))); bufferIds.reserve(viewIds.size());
  for (const int id : viewIds) {
    const auto& view = checked(current.bufferViews, id, "curve witness view");
    budget.add(sizeof(size_t) + 128); budget.copy(view); bufferIds.push_back(view.buffer);
  }
  curveUnique(bufferIds);
  for (const int id : bufferIds) { budget.add(sizeof(CurveBufferMetadata) + 128); budget.copy(checked(current.buffers, id, "curve witness buffer")); }

  // Every retained record/range and construction ID vector is charged before
  // the first owned raw/source/curve copy. Strict decoder scratch has already
  // gone out of scope; physical byte gathering cannot alter source or player.
  auto s = std::make_shared<MaterialAnimationCurveWitness::State>(); s->limits = limits; s->bytes = budget.bytes;
  s->nodes = current.nodes.size(); s->materials = current.materials.size(); s->cameras = current.cameras.size();
  s->lights = current.lights.size(); s->meshes = current.meshes.size(); s->accessors = current.accessors.size();
  s->views = current.bufferViews.size(); s->buffers = current.buffers.size();
  s->requiredExtensions = current.extensionsRequired; s->raw = current.animations;
  s->clips = library_.clips; s->diagnostics = library_.diagnostics;
  s->nodeRows.reserve(ownerCounts[0]); s->materialRows.reserve(ownerCounts[1]);
  s->cameraRows.reserve(ownerCounts[2]); s->lightRows.reserve(ownerCounts[3]);
  for (const auto& owner : owners) {
    const auto append = [&](auto& rows, const auto& source) {
      // TinyGLTF's noexcept move constructor can allocate MSVC map sentinels.
      // Construct in final reserved storage and copy-assign instead.
      rows.emplace_back(); rows.back().index = owner.index; rows.back().record = source[owner.index];
    };
    if (owner.kind == CurveOwnerKind::Node) append(s->nodeRows, current.nodes);
    else if (owner.kind == CurveOwnerKind::Material) append(s->materialRows, current.materials);
    else if (owner.kind == CurveOwnerKind::Camera) append(s->cameraRows, current.cameras);
    else append(s->lightRows, current.lights);
  }
  s->morphRows.reserve(meshIds.size());
  for (const auto mesh : meshIds) {
    CurveMeshShape shape; shape.index = mesh; shape.defaultWeights = current.meshes[mesh].weights.size();
    shape.primitiveTargets.reserve(current.meshes[mesh].primitives.size());
    for (const auto& primitive : current.meshes[mesh].primitives) shape.primitiveTargets.push_back(primitive.targets.size());
    s->morphRows.push_back(std::move(shape));
  }
  s->accessorRows.reserve(accessorIds.size()); s->viewRows.reserve(viewIds.size()); s->bufferRows.reserve(bufferIds.size());
  s->ranges.reserve(rangeCount);
  for (const int id : accessorIds) {
    const auto& accessor = current.accessors[size_t(id)]; s->accessorRows.emplace_back();
    s->accessorRows.back().index = size_t(id); curveCopyAccessor(s->accessorRows.back().record, accessor);
    curveAccessorRanges(current, accessor, [&](int viewId, size_t offset, size_t count, size_t width, size_t stride) {
      const auto& view = current.bufferViews[size_t(viewId)]; CurveEncodedRange range;
      range.buffer = size_t(view.buffer); range.offset = view.byteOffset + offset;
      range.stride = stride; range.count = count; range.width = width; range.bytes.resize(product(count, width));
      const auto& bytes = current.buffers[range.buffer].data;
      for (size_t i = 0; i < count; ++i) std::memcpy(range.bytes.data() + i * width, bytes.data() + range.offset + i * stride, width);
      s->ranges.push_back(std::move(range));
    });
  }
  for (const int id : viewIds) {
    s->viewRows.emplace_back(); s->viewRows.back().index = size_t(id); s->viewRows.back().record = current.bufferViews[size_t(id)];
  }
  for (const int id : bufferIds) {
    s->bufferRows.emplace_back(); auto& row = s->bufferRows.back(); row.index = size_t(id); const auto& buffer = current.buffers[row.index];
    row.dataSize = buffer.data.size(); row.record.name = buffer.name; row.record.uri = buffer.uri; curveCopyMetadata(row.record, buffer);
  }
  MaterialAnimationCurveWitness result; result.state_ = std::move(s);
  if (!result.matches(current, *this)) invalid("material curve witness exceeded its exact context traversal budget");
  return result;
}

namespace {
struct CameraCurveTrack { size_t clip{}; AnimationTrack track; };
struct CameraCurveSampler { size_t clip{}, sampler{}; tinygltf::AnimationSampler record; };
GltfMaterialAnimationEditLimits cameraCopyLimits(const GltfCameraAnimationEditLimits& limits) {
  GltfMaterialAnimationEditLimits copy;
  copy.maxOwnedBytes = limits.maxOwnedBytes; copy.maxDecodedBytes = limits.maxDecodedBytes;
  copy.maxMetadataValues = limits.maxMetadataValues; copy.maxChannels = limits.maxChannels;
  copy.maxClips = limits.maxClips; copy.maxMetadataDepth = limits.maxMetadataDepth;
  return copy;
}
bool selectedCameraTracksMatch(const AnimationLibrary& library, size_t camera,
    std::span<const CameraCurveTrack> retained, const GltfCameraAnimationEditLimits& limits) noexcept {
  if (library.clips.size() > limits.maxClips) return false;
  size_t selected{}; uint64_t tracks{};
  for (size_t c = 0; c < library.clips.size(); ++c) {
    if (library.clips[c].tracks.size() > limits.maxChannels - tracks) return false;
    tracks += library.clips[c].tracks.size();
    for (const auto& track : library.clips[c].tracks)
      if (track.target.kind == AnimationTargetKind::Camera && track.target.index == camera) {
        if (selected >= retained.size() || retained[selected].clip != c ||
            !cloneTrackEqual(retained[selected].track, track, retained[selected].track.target)) return false;
        ++selected;
      }
  }
  return selected == retained.size();
}
} // namespace

struct PreparedCameraRestEdit::State {
  const AnimationPlayer* owner{};
  GltfCameraAnimationEditLimits limits;
  GltfCameraProjectionPlan plan;
  tinygltf::Camera before, after, scratch;
  std::optional<size_t> selected;
  double time{}, speed{};
  bool playing{}, loop{}, consumed{}, float64{};
  uint64_t revision{}, bytes{};
  size_t nodes{}, materials{}, cameras{}, lights{}, clips{};
  std::vector<CameraCurveTrack> tracks;
  std::vector<CameraCurveSampler> samplers;
  std::vector<CurveSourceRow<tinygltf::Accessor>> accessors;
  std::vector<CurveSourceRow<tinygltf::BufferView>> views;
  std::vector<CurveBufferMetadata> buffers;
  std::vector<CurveEncodedRange> ranges;
};
PreparedCameraRestEdit::PreparedCameraRestEdit() noexcept = default;
PreparedCameraRestEdit::~PreparedCameraRestEdit() = default;
PreparedCameraRestEdit::PreparedCameraRestEdit(PreparedCameraRestEdit&&) noexcept = default;
PreparedCameraRestEdit& PreparedCameraRestEdit::operator=(PreparedCameraRestEdit&&) noexcept = default;
PreparedCameraRestEdit::operator bool() const noexcept { return state_ && !state_->consumed; }
uint64_t PreparedCameraRestEdit::ownedBytes() const noexcept { return state_ ? state_->bytes : 0; }

PreparedCameraRestEdit AnimationPlayer::prepareCameraRestEdit(const GltfCameraProjectionPlan& plan,
    GltfCameraAuthoringSourceView authored, const tinygltf::Model& current,
    const GltfCameraAnimationEditLimits& limits) const {
  ValidateGltfCameraProjectionEditBefore(plan, authored, GltfCameraSourceView(current));
  const auto copyLimits = cameraCopyLimits(limits); MaterialAnimationCopyBudget budget(copyLimits);
  if (plan.evaluatedCamera < 0 || size_t(plan.evaluatedCamera) >= library_.restCameras.size() ||
      current.cameras.size() != library_.restCameras.size() || current.nodes.size() != library_.restNodes.size() ||
      current.materials.size() != library_.restMaterials.size() || current.lights.size() != library_.restLights.size() ||
      current.animations.size() != library_.clips.size() || library_.clips.size() > limits.maxClips ||
      cameraRestRevision_ == UINT64_MAX || (selected_ && *selected_ >= library_.clips.size()) ||
      !std::isfinite(time_) || time_ < 0 || !std::isfinite(speed_))
    invalid("camera rest edit has stale source/player identities or controls");
  const auto camera = size_t(plan.evaluatedCamera); const auto& rest = library_.restCameras[camera];
  (void)GltfCameraRecordStorageBytes(rest, limits.authoring);
  (void)GltfCameraRecordStorageBytes(plan.authoredAfter, limits.authoring);
  budget.add(sizeof(PreparedCameraRestEdit::State) + 1024); budget.add(plan.peakOwnedBytes);
  budget.copy(rest); budget.copy(plan.authoredAfter);
  if (!GltfCameraRecordsEqual(rest, plan.authoredBefore) || !GltfCameraProjectionValid(plan.authoredAfter))
    invalid("camera authored rest before witness is stale");
  if (plan.evaluatedAnimationBindings.size() > limits.maxChannels)
    invalid("selected camera curve count exceeds its budget");
  budget.add(product(plan.evaluatedAnimationBindings.size(), sizeof(CameraCurveTrack) + sizeof(CameraCurveSampler) + 2 * sizeof(int)));
  std::vector<int> accessorIds; accessorIds.reserve(product(plan.evaluatedAnimationBindings.size(), 2));
  uint64_t decodedBytes{};
  for (const auto& binding : plan.evaluatedAnimationBindings) {
    const auto& clip = current.animations[binding.clip]; const auto& channel = clip.channels[binding.channel];
    const auto& sampler = checked(clip.samplers, channel.sampler, "selected camera sampler");
    budget.copy(sampler); budget.add(512);
    const auto& input = checked(current.accessors, sampler.input, "selected camera input");
    const auto& output = checked(current.accessors, sampler.output, "selected camera output");
    const auto inputBytes = product(input.count, product(vectorComponents(input.type), sizeof(double)));
    const auto outputBytes = product(output.count, product(vectorComponents(output.type), sizeof(double)));
    if (outputBytes > UINT64_MAX - inputBytes) invalid("selected camera decoded byte arithmetic overflow");
    const auto amount = inputBytes + outputBytes;
    if (amount > limits.maxDecodedBytes - decodedBytes) invalid("selected camera decoded curves exceed their budget");
    decodedBytes += amount; budget.add(amount);
    accessorIds.push_back(sampler.input); accessorIds.push_back(sampler.output);
  }
  curveUnique(accessorIds); std::vector<int> viewIds;
  budget.add(product(accessorIds.size(), 3 * sizeof(int))); viewIds.reserve(product(accessorIds.size(), 3));
  size_t rangeCount{};
  for (const auto id : accessorIds) {
    const auto& accessor = checked(current.accessors, id, "selected camera accessor");
    // The existing checked accessor proves encoded layout; the shared decoder
    // below proves values/shape/precision. Neither operation copies a Model.
    CheckedAccessor checkedAccessor(current, id, "camera curve", {AccessorUsage::Generic,
        HasRequiredExtension(current, "KHR_accessor_float64")});
    (void)checkedAccessor;
    budget.add(sizeof(size_t) + 128); budget.copy(accessor);
    curveAccessorRanges(current, accessor, [&](int view, size_t, size_t count, size_t width, size_t) {
      viewIds.push_back(view); ++rangeCount;
      budget.add(sizeof(CurveEncodedRange) + 64); budget.add(product(count, width));
    });
  }
  curveUnique(viewIds); std::vector<int> bufferIds;
  budget.add(product(viewIds.size(), sizeof(int))); bufferIds.reserve(viewIds.size());
  for (const auto id : viewIds) {
    const auto& view = checked(current.bufferViews, id, "selected camera view");
    budget.add(sizeof(size_t) + 128); budget.copy(view); bufferIds.push_back(view.buffer);
  }
  curveUnique(bufferIds);
  for (const auto id : bufferIds) { budget.add(sizeof(CurveBufferMetadata) + 128); budget.copy(checked(current.buffers, id, "selected camera buffer")); }

  auto s = std::make_unique<PreparedCameraRestEdit::State>(); s->owner = this; s->limits = limits; s->plan = plan;
  s->before = rest; s->after = plan.authoredAfter; s->revision = cameraRestRevision_; s->bytes = budget.bytes;
  s->selected = selected_; s->time = time_; s->speed = speed_; s->playing = playing_; s->loop = loop_;
  s->nodes = library_.restNodes.size(); s->materials = library_.restMaterials.size(); s->cameras = library_.restCameras.size();
  s->lights = library_.restLights.size(); s->clips = library_.clips.size();
  s->float64 = HasRequiredExtension(current, "KHR_accessor_float64");
  s->tracks.reserve(plan.evaluatedAnimationBindings.size()); s->samplers.reserve(plan.evaluatedAnimationBindings.size());
  size_t remaining = size_t(limits.maxDecodedBytes);
  for (const auto& binding : plan.evaluatedAnimationBindings) {
    const auto& channel = current.animations[binding.clip].channels[binding.channel];
    const auto& sampler = current.animations[binding.clip].samplers[size_t(channel.sampler)];
    const auto ext = channel.target_extensions.find("KHR_animation_pointer");
    const auto target = pointerTarget(current, ext->second.Get("pointer").Get<std::string>());
    if (!target || target->kind != AnimationTargetKind::Camera || target->index != camera)
      invalid("selected camera raw target changed during rest preparation");
    s->tracks.push_back({binding.clip, decodeAnimationTrack(current, *target, true, sampler, remaining)});
    s->samplers.emplace_back(); auto& snapshot = s->samplers.back();
    snapshot.clip = binding.clip; snapshot.sampler = size_t(channel.sampler); snapshot.record = sampler;
  }
  if (!selectedCameraTracksMatch(library_, camera, s->tracks, limits)) invalid("selected camera decoded curves changed");
  s->accessors.reserve(accessorIds.size()); s->views.reserve(viewIds.size()); s->buffers.reserve(bufferIds.size()); s->ranges.reserve(rangeCount);
  for (const auto id : accessorIds) {
    const auto& accessor = current.accessors[size_t(id)]; s->accessors.emplace_back();
    s->accessors.back().index = size_t(id); curveCopyAccessor(s->accessors.back().record, accessor);
    curveAccessorRanges(current, accessor, [&](int viewId, size_t offset, size_t count, size_t width, size_t stride) {
      const auto& view = current.bufferViews[size_t(viewId)]; CurveEncodedRange range;
      range.buffer = size_t(view.buffer); range.offset = view.byteOffset + offset; range.count = count;
      range.width = width; range.stride = stride; range.bytes.resize(product(count, width));
      const auto& bytes = current.buffers[range.buffer].data;
      for (size_t i = 0; i < count; ++i) std::memcpy(range.bytes.data() + i * width, bytes.data() + range.offset + i * stride, width);
      s->ranges.push_back(std::move(range));
    });
  }
  for (const auto id : viewIds) { s->views.emplace_back(); s->views.back().index = size_t(id); s->views.back().record = current.bufferViews[size_t(id)]; }
  for (const auto id : bufferIds) {
    s->buffers.emplace_back(); auto& row = s->buffers.back(); row.index = size_t(id); const auto& buffer = current.buffers[row.index];
    row.dataSize = buffer.data.size(); row.record.name = buffer.name; row.record.uri = buffer.uri; curveCopyMetadata(row.record, buffer);
  }
  PreparedCameraRestEdit result; result.state_ = std::move(s);
  if (!cameraRestEditMatches(result, authored, current)) invalid("selected camera rest proof is not current");
  return result;
}

bool AnimationPlayer::cameraRestEditMatches(const PreparedCameraRestEdit& prepared,
    GltfCameraAuthoringSourceView authored, const tinygltf::Model& current) const noexcept {
  if (!prepared.state_) return false;
  const auto& s = *prepared.state_;
  if (s.owner != this || s.consumed || cameraRestRevision_ != s.revision || cameraRestRevision_ == UINT64_MAX ||
      selected_ != s.selected || std::bit_cast<uint64_t>(time_) != std::bit_cast<uint64_t>(s.time) ||
      std::bit_cast<uint64_t>(speed_) != std::bit_cast<uint64_t>(s.speed) || playing_ != s.playing || loop_ != s.loop ||
      library_.restNodes.size() != s.nodes || library_.restMaterials.size() != s.materials || library_.restCameras.size() != s.cameras ||
      library_.restLights.size() != s.lights || library_.clips.size() != s.clips || s.plan.evaluatedCamera < 0 ||
      size_t(s.plan.evaluatedCamera) >= library_.restCameras.size() ||
      !GltfCameraProjectionEditMatches(s.plan, authored, GltfCameraSourceView(current)) ||
      HasRequiredExtension(current, "KHR_accessor_float64") != s.float64 ||
      !selectedCameraTracksMatch(library_, size_t(s.plan.evaluatedCamera), s.tracks, s.limits)) return false;
  CurveExactBudget budget{s.limits.maxMetadataValues, s.limits.maxMetadataDepth};
  if (!curveExact(library_.restCameras[size_t(s.plan.evaluatedCamera)], s.before, budget)) return false;
  for (const auto& sampler : s.samplers) {
    if (sampler.clip >= current.animations.size() || sampler.sampler >= current.animations[sampler.clip].samplers.size() ||
        !curveExact(current.animations[sampler.clip].samplers[sampler.sampler], sampler.record, budget)) return false;
  }
  const auto rows = [&](const auto& source, const auto& retained) {
    for (const auto& row : retained) if (row.index >= source.size() || !curveExact(source[row.index], row.record, budget)) return false;
    return true;
  };
  if (!rows(current.accessors, s.accessors) || !rows(current.bufferViews, s.views)) return false;
  for (const auto& row : s.buffers)
    if (row.index >= current.buffers.size() || current.buffers[row.index].data.size() != row.dataSize ||
        !curveExact(current.buffers[row.index], row.record, budget)) return false;
  for (const auto& range : s.ranges) {
    if (range.buffer >= current.buffers.size()) return false;
    const auto& bytes = current.buffers[range.buffer].data;
    if (range.offset > bytes.size() || range.width > bytes.size() - range.offset || !range.count ||
        range.count - 1 > (bytes.size() - range.offset - range.width) / range.stride) return false;
    for (size_t i = 0; i < range.count; ++i)
      if (std::memcmp(bytes.data() + range.offset + i * range.stride, range.bytes.data() + i * range.width, range.width) != 0) return false;
  }
  return true;
}
bool AnimationPlayer::exchangeCameraRestEdit(PreparedCameraRestEdit& prepared,
    GltfCameraAuthoringSourceView authored, const tinygltf::Model& current) noexcept {
  if (!cameraRestEditMatches(prepared, authored, current)) return false;
  auto& s = *prepared.state_;
  if (!s.plan.changed) { s.consumed = true; return true; }
  static_assert(std::is_nothrow_move_assignable_v<tinygltf::Camera>);
  auto& live = library_.restCameras[size_t(s.plan.evaluatedCamera)];
  s.scratch = std::move(live); live = std::move(s.after); s.after = std::move(s.scratch);
  ++cameraRestRevision_; s.consumed = true;
  return true;
}

namespace {
struct CameraNodeRestPose {
  std::vector<double> matrix, translation, rotation, scale;
  void copy(const tinygltf::Node& node) {
    matrix = node.matrix; translation = node.translation; rotation = node.rotation; scale = node.scale;
  }
  bool matches(const tinygltf::Node& node) const noexcept {
    CurveExactBudget budget{};
    return curveExact(matrix, node.matrix, budget) && curveExact(translation, node.translation, budget) &&
        curveExact(rotation, node.rotation, budget) && curveExact(scale, node.scale, budget);
  }
  void exchange(tinygltf::Node& node) noexcept {
    matrix.swap(node.matrix); translation.swap(node.translation); rotation.swap(node.rotation); scale.swap(node.scale);
  }
};
GltfMaterialAnimationEditLimits cameraNodeCopyLimits(const GltfCameraNodeAnimationEditLimits& limits) {
  GltfMaterialAnimationEditLimits copy;
  copy.maxOwnedBytes = limits.maxOwnedBytes; copy.maxDecodedBytes = limits.maxDecodedBytes;
  copy.maxMetadataValues = limits.maxMetadataValues; copy.maxChannels = limits.maxChannels;
  copy.maxClips = limits.maxClips; copy.maxMetadataDepth = limits.maxMetadataDepth;
  return copy;
}
bool selectedNodeTracksMatch(const AnimationLibrary& library, size_t node,
    std::span<const CameraCurveTrack> retained, const GltfCameraNodeAnimationEditLimits& limits) noexcept {
  if (library.clips.size() > limits.maxClips) return false;
  size_t selected{}; uint64_t tracks{};
  for (size_t c = 0; c < library.clips.size(); ++c) {
    if (library.clips[c].tracks.size() > limits.maxChannels - tracks) return false;
    tracks += library.clips[c].tracks.size();
    for (const auto& track : library.clips[c].tracks) {
      const auto kind = track.target.kind;
      if (track.target.index != node || (kind != AnimationTargetKind::NodeTranslation &&
          kind != AnimationTargetKind::NodeRotation && kind != AnimationTargetKind::NodeScale &&
          kind != AnimationTargetKind::NodeWeights)) continue;
      if (kind != AnimationTargetKind::NodeWeights || selected >= retained.size() || retained[selected].clip != c ||
          !cloneTrackEqual(retained[selected].track, track, retained[selected].track.target)) return false;
      ++selected;
    }
  }
  return selected == retained.size();
}
struct CameraNodeCurveChannel { size_t clip{}, channel{}; tinygltf::AnimationChannel record; };
} // namespace

struct PreparedCameraNodeRestEdit::State {
  const AnimationPlayer* owner{};
  GltfCameraNodeAnimationEditLimits limits;
  GltfCameraPosePlan plan;
  CameraNodeRestPose before, after;
  std::optional<size_t> selected;
  double time{}, speed{};
  bool playing{}, loop{}, consumed{}, float64{};
  uint64_t revision{}, bytes{};
  size_t nodes{}, materials{}, cameras{}, lights{}, clips{};
  int mesh{-1};
  size_t nodeWeights{}, meshWeights{};
  std::vector<size_t> primitiveTargets;
  std::vector<CameraCurveTrack> tracks;
  std::vector<CameraCurveSampler> samplers;
  std::vector<CameraNodeCurveChannel> channels;
  std::vector<CurveSourceRow<tinygltf::Accessor>> accessors;
  std::vector<CurveSourceRow<tinygltf::BufferView>> views;
  std::vector<CurveBufferMetadata> buffers;
  std::vector<CurveEncodedRange> ranges;
};
PreparedCameraNodeRestEdit::PreparedCameraNodeRestEdit() noexcept = default;
PreparedCameraNodeRestEdit::~PreparedCameraNodeRestEdit() = default;
PreparedCameraNodeRestEdit::PreparedCameraNodeRestEdit(PreparedCameraNodeRestEdit&&) noexcept = default;
PreparedCameraNodeRestEdit& PreparedCameraNodeRestEdit::operator=(PreparedCameraNodeRestEdit&&) noexcept = default;
PreparedCameraNodeRestEdit::operator bool() const noexcept { return state_ && !state_->consumed; }
uint64_t PreparedCameraNodeRestEdit::ownedBytes() const noexcept { return state_ ? state_->bytes : 0; }

PreparedCameraNodeRestEdit AnimationPlayer::prepareCameraNodeRestEdit(const GltfCameraPosePlan& plan,
    GltfCameraPoseSourceView authored, const tinygltf::Model& current,
    const GltfCameraNodeAnimationEditLimits& limits) const {
  if (!GltfCameraPoseEditMatches(plan, authored, GltfCameraPoseView(current), plan.graphBefore))
    invalid("camera node pose receipt or source is stale");
  const auto copyLimits = cameraNodeCopyLimits(limits); MaterialAnimationCopyBudget budget(copyLimits);
  if (limits.maxDecodedBytes > SIZE_MAX || plan.evaluatedNode < 0 || size_t(plan.evaluatedNode) >= library_.restNodes.size() ||
      authored.nodes.size() > limits.authoring.maxNodes || current.nodes.size() > limits.authoring.maxNodes ||
      authored.cameras.size() > limits.authoring.maxCameraRecords || current.cameras.size() > limits.authoring.maxCameraRecords ||
      authored.animations.size() > limits.authoring.maxAnimationClips || current.animations.size() > limits.authoring.maxAnimationClips ||
      plan.graphBefore.nodeCount > limits.authoring.maxGraphNodes ||
      current.nodes.size() != library_.restNodes.size() || current.materials.size() != library_.restMaterials.size() ||
      current.cameras.size() != library_.restCameras.size() || current.lights.size() != library_.restLights.size() ||
      current.animations.size() != library_.clips.size() || library_.clips.size() > limits.maxClips ||
      cameraNodeRestRevision_ == UINT64_MAX || (selected_ && *selected_ >= library_.clips.size()) ||
      !std::isfinite(time_) || time_ < 0 || !std::isfinite(speed_))
    invalid("camera node rest edit has stale source/player identities or controls");
  const auto node = size_t(plan.evaluatedNode); const auto& rest = library_.restNodes[node];
  (void)GltfCameraPoseNodeStorageBytes(plan.authoredBefore, limits.authoring);
  (void)GltfCameraPoseNodeStorageBytes(plan.authoredAfter, limits.authoring);
  (void)GltfCameraPoseNodeStorageBytes(plan.evaluatedBefore, limits.authoring);
  (void)GltfCameraPoseNodeStorageBytes(plan.evaluatedAfter, limits.authoring);
  for (const auto source : {authored.animations, std::span<const tinygltf::Animation>(current.animations)}) {
    uint64_t channels{};
    for (const auto& clip : source) {
      if (clip.channels.size() > limits.authoring.maxAnimationChannels - channels)
        invalid("camera node source channels exceed their authoring budget");
      channels += clip.channels.size();
      for (const auto& channel : clip.channels) if (channel.target_path == "pointer") {
        const auto extension = std::find_if(channel.target_extensions.begin(), channel.target_extensions.end(),
            [](const auto& item) { return item.first == "KHR_animation_pointer"; });
        if (extension == channel.target_extensions.end() || !extension->second.IsObject() ||
            !extension->second.Get("pointer").IsString() ||
            extension->second.Get("pointer").Get<std::string>().size() > limits.authoring.maxPointerBytes)
          invalid("camera node source pointer exceeds its authoring budget");
      }
    }
  }
  budget.add(sizeof(PreparedCameraNodeRestEdit::State) + 1024); budget.add(plan.peakOwnedBytes);
  // The plan contains selected source records; the rest proof owns only pose
  // arrays. No rest metadata/weights or whole source/library is copied.
  budget.copy(plan.authoredBefore); budget.copy(plan.authoredAfter);
  budget.copy(plan.evaluatedBefore); budget.copy(plan.evaluatedAfter);
  budget.copy(rest.matrix); budget.copy(rest.translation); budget.copy(rest.rotation); budget.copy(rest.scale);
  budget.copy(plan.authoredAfter.matrix); budget.copy(plan.authoredAfter.translation);
  budget.copy(plan.authoredAfter.rotation); budget.copy(plan.authoredAfter.scale);
  CameraNodeRestPose restBefore; restBefore.copy(rest);
  if (!restBefore.matches(plan.authoredBefore)) invalid("camera node authored rest before witness is stale");
  const auto count = plan.evaluatedAnimationBindings.size();
  if (count > limits.maxChannels) invalid("selected camera node channel count exceeds its budget");
  budget.add(product(count, sizeof(CameraCurveTrack) + sizeof(CameraCurveSampler) + sizeof(CameraNodeCurveChannel) + 2 * sizeof(int)));
  std::vector<int> accessorIds; accessorIds.reserve(product(count, 2));
  uint64_t decodedBytes{};
  for (const auto& binding : plan.evaluatedAnimationBindings) {
    if (binding.clip >= current.animations.size() || binding.channel >= current.animations[binding.clip].channels.size())
      invalid("selected camera node raw channel is unavailable");
    const auto& clip = current.animations[binding.clip]; const auto& channel = clip.channels[binding.channel];
    const auto& sampler = checked(clip.samplers, channel.sampler, "selected camera node sampler");
    budget.copy(channel); budget.copy(sampler); budget.add(512);
    const auto& input = checked(current.accessors, sampler.input, "selected camera node input");
    const auto& output = checked(current.accessors, sampler.output, "selected camera node output");
    const auto inputBytes = product(input.count, product(vectorComponents(input.type), sizeof(double)));
    const auto outputBytes = product(output.count, product(vectorComponents(output.type), sizeof(double)));
    if (outputBytes > UINT64_MAX - inputBytes) invalid("selected camera node decoded byte arithmetic overflow");
    const auto amount = inputBytes + outputBytes;
    if (amount > limits.maxDecodedBytes - decodedBytes) invalid("selected camera node decoded curves exceed their budget");
    decodedBytes += amount; budget.add(amount);
    accessorIds.push_back(sampler.input); accessorIds.push_back(sampler.output);
  }
  if (count) {
    const auto& mesh = checked(current.meshes, current.nodes[node].mesh, "selected camera node morph mesh");
    budget.add(product(mesh.primitives.size(), sizeof(size_t)));
  }
  curveUnique(accessorIds); std::vector<int> viewIds;
  budget.add(product(accessorIds.size(), 3 * sizeof(int))); viewIds.reserve(product(accessorIds.size(), 3));
  size_t rangeCount{};
  for (const auto id : accessorIds) {
    const auto& accessor = checked(current.accessors, id, "selected camera node accessor");
    CheckedAccessor checkedAccessor(current, id, "camera node curve", {AccessorUsage::Generic,
        HasRequiredExtension(current, "KHR_accessor_float64")});
    (void)checkedAccessor; budget.add(sizeof(size_t) + 128); budget.copy(accessor);
    curveAccessorRanges(current, accessor, [&](int view, size_t, size_t elements, size_t width, size_t) {
      viewIds.push_back(view); ++rangeCount;
      budget.add(sizeof(CurveEncodedRange) + 64); budget.add(product(elements, width));
    });
  }
  curveUnique(viewIds); std::vector<int> bufferIds;
  budget.add(product(viewIds.size(), sizeof(int))); bufferIds.reserve(viewIds.size());
  for (const auto id : viewIds) {
    const auto& view = checked(current.bufferViews, id, "selected camera node view");
    budget.add(sizeof(size_t) + 128); budget.copy(view); bufferIds.push_back(view.buffer);
  }
  curveUnique(bufferIds);
  for (const auto id : bufferIds) { budget.add(sizeof(CurveBufferMetadata) + 128); budget.copy(checked(current.buffers, id, "selected camera node buffer")); }

  auto s = std::make_unique<PreparedCameraNodeRestEdit::State>(); s->owner = this; s->limits = limits; s->plan = plan;
  s->before = std::move(restBefore); s->after.copy(plan.authoredAfter); s->revision = cameraNodeRestRevision_; s->bytes = budget.bytes;
  s->selected = selected_; s->time = time_; s->speed = speed_; s->playing = playing_; s->loop = loop_;
  s->nodes = library_.restNodes.size(); s->materials = library_.restMaterials.size(); s->cameras = library_.restCameras.size();
  s->lights = library_.restLights.size(); s->clips = library_.clips.size();
  s->float64 = HasRequiredExtension(current, "KHR_accessor_float64");
  s->tracks.reserve(count); s->samplers.reserve(count); s->channels.reserve(count);
  size_t remaining = size_t(limits.maxDecodedBytes);
  for (const auto& binding : plan.evaluatedAnimationBindings) {
    const auto& channel = current.animations[binding.clip].channels[binding.channel];
    const auto& sampler = current.animations[binding.clip].samplers[size_t(channel.sampler)];
    const bool pointer = channel.target_path == "pointer";
    std::optional<AnimationTarget> target;
    if (pointer) {
      const auto ext = channel.target_extensions.find("KHR_animation_pointer");
      if (ext == channel.target_extensions.end() || !ext->second.IsObject() || !ext->second.Get("pointer").IsString())
        invalid("selected camera node raw pointer is malformed");
      target = pointerTarget(current, ext->second.Get("pointer").Get<std::string>());
    } else if (channel.target_node >= 0) target = nodeTarget(current, size_t(channel.target_node), channel.target_path);
    if (!target || target->kind != AnimationTargetKind::NodeWeights || target->index != node)
      invalid("camera node pose is referenced by an animated or unsupported selected target");
    s->tracks.push_back({binding.clip, decodeAnimationTrack(current, *target, pointer, sampler, remaining)});
    s->samplers.emplace_back(); auto& sampled = s->samplers.back();
    sampled.clip = binding.clip; sampled.sampler = size_t(channel.sampler); sampled.record = sampler;
    s->channels.emplace_back(); auto& raw = s->channels.back();
    raw.clip = binding.clip; raw.channel = binding.channel; raw.record = channel;
  }
  if (!selectedNodeTracksMatch(library_, node, s->tracks, limits)) invalid("selected camera node decoded curves changed");
  if (count) {
    s->mesh = current.nodes[node].mesh; s->nodeWeights = current.nodes[node].weights.size();
    const auto& mesh = current.meshes[size_t(s->mesh)]; s->meshWeights = mesh.weights.size();
    s->primitiveTargets.reserve(mesh.primitives.size());
    for (const auto& primitive : mesh.primitives) s->primitiveTargets.push_back(primitive.targets.size());
  }
  s->accessors.reserve(accessorIds.size()); s->views.reserve(viewIds.size()); s->buffers.reserve(bufferIds.size()); s->ranges.reserve(rangeCount);
  for (const auto id : accessorIds) {
    const auto& accessor = current.accessors[size_t(id)]; s->accessors.emplace_back();
    s->accessors.back().index = size_t(id); curveCopyAccessor(s->accessors.back().record, accessor);
    curveAccessorRanges(current, accessor, [&](int viewId, size_t offset, size_t elements, size_t width, size_t stride) {
      const auto& view = current.bufferViews[size_t(viewId)]; CurveEncodedRange range;
      range.buffer = size_t(view.buffer); range.offset = view.byteOffset + offset; range.count = elements;
      range.width = width; range.stride = stride; range.bytes.resize(product(elements, width));
      const auto& bytes = current.buffers[range.buffer].data;
      for (size_t i = 0; i < elements; ++i) std::memcpy(range.bytes.data() + i * width, bytes.data() + range.offset + i * stride, width);
      s->ranges.push_back(std::move(range));
    });
  }
  for (const auto id : viewIds) { s->views.emplace_back(); s->views.back().index = size_t(id); s->views.back().record = current.bufferViews[size_t(id)]; }
  for (const auto id : bufferIds) {
    s->buffers.emplace_back(); auto& row = s->buffers.back(); row.index = size_t(id); const auto& buffer = current.buffers[row.index];
    row.dataSize = buffer.data.size(); row.record.name = buffer.name; row.record.uri = buffer.uri; curveCopyMetadata(row.record, buffer);
  }
  PreparedCameraNodeRestEdit result; result.state_ = std::move(s);
  if (!cameraNodeRestEditMatches(result, authored, current)) invalid("selected camera node rest proof is not current");
  return result;
}

bool AnimationPlayer::cameraNodeRestEditMatches(const PreparedCameraNodeRestEdit& prepared,
    GltfCameraPoseSourceView authored, const tinygltf::Model& current) const noexcept {
  if (!prepared.state_) return false;
  const auto& s = *prepared.state_;
  if (s.owner != this || s.consumed || cameraNodeRestRevision_ != s.revision || cameraNodeRestRevision_ == UINT64_MAX ||
      selected_ != s.selected || std::bit_cast<uint64_t>(time_) != std::bit_cast<uint64_t>(s.time) ||
      std::bit_cast<uint64_t>(speed_) != std::bit_cast<uint64_t>(s.speed) || playing_ != s.playing || loop_ != s.loop ||
      current.nodes.size() != s.nodes || current.materials.size() != s.materials || current.cameras.size() != s.cameras ||
      current.lights.size() != s.lights || current.animations.size() != s.clips ||
      library_.restNodes.size() != s.nodes || library_.restMaterials.size() != s.materials || library_.restCameras.size() != s.cameras ||
      library_.restLights.size() != s.lights || library_.clips.size() != s.clips || s.plan.evaluatedNode < 0 ||
      size_t(s.plan.evaluatedNode) >= library_.restNodes.size() ||
      !GltfCameraPoseEditMatches(s.plan, authored, GltfCameraPoseView(current), s.plan.graphBefore) ||
      HasRequiredExtension(current, "KHR_accessor_float64") != s.float64 ||
      !selectedNodeTracksMatch(library_, size_t(s.plan.evaluatedNode), s.tracks, s.limits) ||
      !s.before.matches(library_.restNodes[size_t(s.plan.evaluatedNode)])) return false;
  if (!s.tracks.empty()) {
    const auto& node = current.nodes[size_t(s.plan.evaluatedNode)];
    if (node.mesh != s.mesh || node.weights.size() != s.nodeWeights || s.mesh < 0 || size_t(s.mesh) >= current.meshes.size()) return false;
    const auto& mesh = current.meshes[size_t(s.mesh)];
    if (mesh.weights.size() != s.meshWeights || mesh.primitives.size() != s.primitiveTargets.size()) return false;
    for (size_t i = 0; i < mesh.primitives.size(); ++i) if (mesh.primitives[i].targets.size() != s.primitiveTargets[i]) return false;
  }
  CurveExactBudget budget{s.limits.maxMetadataValues, s.limits.maxMetadataDepth};
  for (const auto& channel : s.channels)
    if (channel.clip >= current.animations.size() || channel.channel >= current.animations[channel.clip].channels.size() ||
        !curveExact(current.animations[channel.clip].channels[channel.channel], channel.record, budget)) return false;
  for (const auto& sampler : s.samplers)
    if (sampler.clip >= current.animations.size() || sampler.sampler >= current.animations[sampler.clip].samplers.size() ||
        !curveExact(current.animations[sampler.clip].samplers[sampler.sampler], sampler.record, budget)) return false;
  const auto rows = [&](const auto& source, const auto& retained) {
    for (const auto& row : retained) if (row.index >= source.size() || !curveExact(source[row.index], row.record, budget)) return false;
    return true;
  };
  if (!rows(current.accessors, s.accessors) || !rows(current.bufferViews, s.views)) return false;
  for (const auto& row : s.buffers)
    if (row.index >= current.buffers.size() || current.buffers[row.index].data.size() != row.dataSize ||
        !curveExact(current.buffers[row.index], row.record, budget)) return false;
  for (const auto& range : s.ranges) {
    if (range.buffer >= current.buffers.size()) return false;
    const auto& bytes = current.buffers[range.buffer].data;
    if (range.offset > bytes.size() || range.width > bytes.size() - range.offset || !range.count || !range.stride ||
        range.count - 1 > (bytes.size() - range.offset - range.width) / range.stride) return false;
    for (size_t i = 0; i < range.count; ++i)
      if (std::memcmp(bytes.data() + range.offset + i * range.stride, range.bytes.data() + i * range.width, range.width) != 0) return false;
  }
  return true;
}
bool AnimationPlayer::exchangeCameraNodeRestEdit(PreparedCameraNodeRestEdit& prepared,
    GltfCameraPoseSourceView authored, const tinygltf::Model& current) noexcept {
  if (!cameraNodeRestEditMatches(prepared, authored, current)) return false;
  auto& s = *prepared.state_;
  if (s.plan.changed) {
    s.after.exchange(library_.restNodes[size_t(s.plan.evaluatedNode)]);
    ++cameraNodeRestRevision_;
  }
  s.consumed = true;
  return true;
}

tinygltf::Material AnimationPlayer::evaluateMaterialRestEdit(size_t materialId,
    const tinygltf::Material& record, const GltfMaterialAnimationEditLimits& limits) const {
  MaterialAnimationCopyBudget budget(limits);
  if (materialId >= library_.restMaterials.size()) invalid("material sample identity is out of range");
  budget.copy(record); budget.copy(record);
  if(library_.clips.size()>limits.maxClips) invalid("material sample clip budget exceeded");
  if(selected_ && (*selected_>=library_.clips.size() || !std::isfinite(time_))) invalid("invalid material animation clock");
  uint64_t channels{};bool targeted=false;
  for(const auto& clip:library_.clips) {
    if(clip.tracks.size()>limits.maxChannels-channels) invalid("material sample channel budget exceeded");
    channels+=clip.tracks.size();
    for(const auto& track:clip.tracks) if(track.target.kind==AnimationTargetKind::Material && track.target.index==materialId) {
      if(!track.target.components || track.target.components>4 ||
          (track.target.element && (track.target.components==1 || *track.target.element>=track.target.components)))
        invalid("invalid material rest target shape");
      targeted=true;
      budget.add(sizeof(AnimationTarget)+1024);budget.string(track.target.property);
      budget.add(product(track.target.property.size(),4));
    }
  }
  const auto& rest=library_.restMaterials[materialId];
  if(targeted) {budget.copy(rest);budget.copy(rest);}
  AnimationPose pose;
  if (selected_) {
    const auto& clip = library_.clips[*selected_];
    for (const auto& track : clip.tracks) if (track.target.kind == AnimationTargetKind::Material && track.target.index == materialId) {
      const auto width = sampleComponents(track.target);
      const auto expected = product(product(track.times.size(), width),
          track.interpolation == AnimationInterpolation::CubicSpline ? 3 : 1);
      if (track.times.empty() || !width || expected != track.values.size()) invalid("invalid material animation track storage");
      // Includes the detached property's map/array allocation during apply.
      budget.add(sizeof(AnimatedValue) + 512); budget.string(track.target.property);
      budget.add(product(width, sizeof(double)));
      pose.values.push_back({track.target, sampleTrack(track, time_)});
    }
  }
  validatePoseValues(pose);
  auto result = record;
  std::array<bool,6> caches{};
  for(const auto& clip:library_.clips) for(const auto& track:clip.tracks)
    if(track.target.kind==AnimationTargetKind::Material && track.target.index==materialId) {
      restoreMaterialTarget(result,rest,track.target);
      if(const auto cache=materialCacheOwner(track.target.property)) caches[*cache]=true;
    }
  for (const auto& sample : pose.values) applyMaterial(result, sample);
  // Reconcile each affected raw cache once, rather than scanning a potentially
  // large extension map for every track. Untracked metadata remains untouched.
  reconcileMaterialRestCaches(result,record,&rest,caches);
  return result;
}

void AnimationPlayer::setSpeed(double speed) {
  if (!std::isfinite(speed)) invalid("animation speed must be finite");
  speed_ = speed;
}
AnimationDirtySet AnimationPlayer::selectClip(tinygltf::Model& model, size_t clip) {
  if (clip >= library_.clips.size()) invalid("animation clip index out of range");
  const auto pose = SampleAnimation(library_.clips[clip],0);
  validatePoseValues(pose); // A failed selection must not restore the old clip first.
  AnimationTransaction transaction(model); transaction.capture(library_);
  MaterialCacheMasks caches;
  auto dirty = restoreRestPose(model,library_,caches);
  dirty.merge(applyPose(model,pose,caches));
  reconcileMaterialCaches(model,caches,transaction,&library_);
  transaction.commit();
  selected_ = clip; time_ = 0;
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
