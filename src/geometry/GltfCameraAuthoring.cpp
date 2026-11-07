#include <Container/geometry/GltfCameraAuthoring.h>

#include <Container/common/CommonMath.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <string>
#include <string_view>

namespace container::geometry::gltf {
namespace {
using P = GltfCameraProperty;
using V = tinygltf::Value;
constexpr size_t propertyCount = size_t(P::Count);
constexpr std::array<std::string_view, propertyCount> paths{
    "perspective/yfov", "perspective/aspectRatio", "perspective/znear", "perspective/zfar",
    "orthographic/xmag", "orthographic/ymag", "orthographic/znear", "orthographic/zfar"};

[[noreturn]] void fail(const char* reason) {
  throw std::invalid_argument(std::string("glTF camera authoring: ") + reason);
}
bool exact(double a, double b) noexcept {
  return std::bit_cast<uint64_t>(a) == std::bit_cast<uint64_t>(b);
}
bool validStorage(const V& v) noexcept {
  return (v.IsString() || v.Get<std::string>().empty()) &&
      (v.IsBinary() || v.Get<std::vector<unsigned char>>().empty()) &&
      (v.IsArray() || v.Get<V::Array>().empty()) &&
      (v.IsObject() || v.Get<V::Object>().empty()) &&
      (v.IsString() || v.IsBinary() || v.IsArray() || v.IsObject() ||
       v.IsBool() || v.IsNumber() || v.Type() == tinygltf::NULL_TYPE);
}
bool exactValue(const V& a, const V& b) noexcept {
  if (a.Type() != b.Type() || !validStorage(a) || !validStorage(b)) return false;
  if (a.IsInt()) return a.Get<int>() == b.Get<int>();
  if (a.IsReal()) return exact(a.Get<double>(), b.Get<double>());
  if (a.IsBool()) return a.Get<bool>() == b.Get<bool>();
  if (a.IsString()) return a.Get<std::string>() == b.Get<std::string>();
  if (a.IsBinary()) return a.Get<std::vector<unsigned char>>() == b.Get<std::vector<unsigned char>>();
  if (a.IsArray()) {
    const auto& x = a.Get<V::Array>(); const auto& y = b.Get<V::Array>();
    if (x.size() != y.size()) return false;
    for (size_t i = 0; i < x.size(); ++i) if (!exactValue(x[i], y[i])) return false;
  } else if (a.IsObject()) {
    const auto& x = a.Get<V::Object>(); const auto& y = b.Get<V::Object>();
    if (x.size() != y.size()) return false;
    auto next = y.begin();
    for (const auto& [key, value] : x) {
      if (key != next->first || !exactValue(value, next->second)) return false;
      ++next;
    }
  }
  return true;
}
bool exactExtensions(const tinygltf::ExtensionMap& a, const tinygltf::ExtensionMap& b) noexcept {
  if (a.size() != b.size()) return false;
  auto next = b.begin();
  for (const auto& [key, value] : a) {
    if (key != next->first || !exactValue(value, next->second)) return false;
    ++next;
  }
  return true;
}
template<class T> bool exactMetadata(const T& a, const T& b) noexcept {
  return exactValue(a.extras, b.extras) && exactExtensions(a.extensions, b.extensions) &&
      a.extras_json_string == b.extras_json_string && a.extensions_json_string == b.extensions_json_string;
}
std::array<double, propertyCount> scalars(const tinygltf::Camera& c) noexcept {
  return {c.perspective.yfov, c.perspective.aspectRatio, c.perspective.znear, c.perspective.zfar,
      c.orthographic.xmag, c.orthographic.ymag, c.orthographic.znear, c.orthographic.zfar};
}
bool exactCamera(const tinygltf::Camera& a, const tinygltf::Camera& b) noexcept {
  if (a.type != b.type || a.name != b.name || !exactMetadata(a, b) ||
      !exactMetadata(a.perspective, b.perspective) || !exactMetadata(a.orthographic, b.orthographic)) return false;
  const auto x = scalars(a), y = scalars(b);
  for (size_t i = 0; i < propertyCount; ++i) if (!exact(x[i], y[i])) return false;
  return true;
}
bool exactChannel(const tinygltf::AnimationChannel& a, const tinygltf::AnimationChannel& b) noexcept {
  return a.sampler == b.sampler && a.target_node == b.target_node && a.target_path == b.target_path &&
      exactMetadata(a, b) && exactValue(a.target_extras, b.target_extras) &&
      exactExtensions(a.target_extensions, b.target_extensions) &&
      a.target_extras_json_string == b.target_extras_json_string &&
      a.target_extensions_json_string == b.target_extensions_json_string;
}
bool validLimits(const GltfCameraAuthoringLimits& l) noexcept {
  // An explicit hard stack bound also applies when a caller widens its budget.
  return l.maxOwnedBytes != 0 && l.maxMetadataDepth <= 256;
}
struct Budget {
  const GltfCameraAuthoringLimits& limits;
  uint64_t bytes{}, values{};
  bool add(uint64_t n) noexcept {
    if (bytes > limits.maxOwnedBytes || n > limits.maxOwnedBytes - bytes) return false;
    bytes += n; return true;
  }
  bool array(uint64_t count, uint64_t stride) noexcept {
    return count <= UINT64_MAX / stride && add(count * stride);
  }
  bool value(const V& v, uint32_t depth = 0) noexcept {
    if (depth > limits.maxMetadataDepth || values == limits.maxMetadataValues || !validStorage(v)) return false;
    ++values;
    if (!add(sizeof(V))) return false;
    if (v.IsString()) return add(v.Get<std::string>().size());
    if (v.IsBinary()) return add(v.Get<std::vector<unsigned char>>().size());
    if (v.IsNumber()) return std::isfinite(v.GetNumberAsDouble());
    if (v.IsArray()) {
      for (const auto& child : v.Get<V::Array>()) if (!value(child, depth + 1)) return false;
    } else if (v.IsObject()) {
      for (const auto& [key, child] : v.Get<V::Object>())
        if (!add(128) || !add(key.size()) || !value(child, depth + 1)) return false;
    }
    return true;
  }
  bool extensions(const tinygltf::ExtensionMap& map) noexcept {
    for (const auto& [key, v] : map)
      if (!add(128) || !add(key.size()) || !value(v)) return false;
    return true;
  }
  template<class T> bool metadata(const T& t) noexcept {
    return add(t.extras_json_string.size()) && add(t.extensions_json_string.size()) &&
        value(t.extras) && extensions(t.extensions);
  }
  bool camera(const tinygltf::Camera& c) noexcept {
    return add(sizeof(c)) && add(c.name.size()) && add(c.type.size()) &&
        metadata(c) && metadata(c.perspective) && metadata(c.orthographic);
  }
  bool channel(const tinygltf::AnimationChannel& c) noexcept {
    return add(sizeof(c)) && add(c.target_path.size()) && metadata(c) && value(c.target_extras) &&
        extensions(c.target_extensions) && add(c.target_extras_json_string.size()) &&
        add(c.target_extensions_json_string.size());
  }
};
struct Projection {
  GltfCameraProjection type{};
  std::array<double, propertyCount> values{};
};
bool projection(const tinygltf::Camera& c, Projection& p) noexcept {
  if (c.type == "perspective") p.type = GltfCameraProjection::Perspective;
  else if (c.type == "orthographic") p.type = GltfCameraProjection::Orthographic;
  else return false;
  p.values = scalars(c);
  return true;
}
bool validProjection(const Projection& p) noexcept {
  for (double v : p.values) if (!std::isfinite(v)) return false;
  glm::mat4 matrix;
  if (p.type == GltfCameraProjection::Perspective) {
    const double yfov = p.values[0], aspect = p.values[1], near = p.values[2], far = p.values[3];
    const float fov = float(yfov), a = aspect == 0 ? 1.f : float(aspect), n = float(near);
    const float f = far == 0 ? std::max(1e6f, n * 2.f) : float(far);
    if (!(yfov > 0 && yfov < std::numbers::pi) || aspect < 0 || near <= 0 || far < 0 ||
        (far != 0 && far <= near) || !(fov > 0 && fov < std::numbers::pi_v<float>) ||
        !std::isfinite(a) || a <= 0 || !std::isfinite(n) || n <= 0 || !std::isfinite(f) || f <= n) return false;
    matrix = container::math::perspectiveRH_ReverseZ(fov, a, n, f);
    if (far == 0) { matrix[2][2] = 0; matrix[3][2] = n; }
  } else if (p.type == GltfCameraProjection::Orthographic) {
    const float x = float(p.values[4]), y = float(p.values[5]), n = float(p.values[6]), f = float(p.values[7]);
    if (!(p.values[4] > 0 && p.values[5] > 0 && p.values[6] >= 0 && p.values[7] > p.values[6]) ||
        !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(n) || !std::isfinite(f) ||
        x <= 0 || y <= 0 || n < 0 || f <= n) return false;
    matrix = container::math::orthoRH_ReverseZ(-x, x, -y, y, n, f);
  } else return false;
  // Match the existing RH/reverse-Z/no-Y-flip evaluator, without accepting a
  // finite double whose binary32 projection overflows or becomes singular.
  for (int column = 0; column < 4; ++column) for (int row = 0; row < 4; ++row)
    if (!std::isfinite(matrix[column][row])) return false;
  return matrix[0][0] > 0 && matrix[1][1] > 0 &&
      (p.type == GltfCameraProjection::Perspective ? matrix[3][2] > 0 : matrix[2][2] > 0);
}
bool patchProjection(Projection& p, const GltfCameraProjectionPatch& patch,
    const GltfCameraAuthoringLimits& limits) noexcept {
  if (patch.values.size() > limits.maxPatchOperations) return false;
  const auto originalType = p.type;
  if (patch.type) {
    if (*patch.type != GltfCameraProjection::Perspective && *patch.type != GltfCameraProjection::Orthographic) return false;
    p.type = *patch.type;
  }
  std::array<bool, propertyCount> seen{};
  for (const auto& edit : patch.values) {
    const auto index = size_t(edit.property);
    if (index >= propertyCount || seen[index] || (index < 4) != (p.type == GltfCameraProjection::Perspective)) return false;
    seen[index] = true;
    if (const auto* v = std::get_if<double>(&edit.value)) {
      // Numeric zero is not an alternative spelling for omission in the UI.
      if ((index == 1 || index == 3) && *v <= 0) return false;
      p.values[index] = *v;
    } else {
      if (index != 1 && index != 3) return false;
      p.values[index] = 0;
    }
  }
  if (p.type != originalType) {
    if (p.type == GltfCameraProjection::Perspective) {
      if (!seen[0] || !seen[2]) return false;
    } else if (!seen[4] || !seen[5] || !seen[6] || !seen[7]) return false;
  }
  return true;
}
void assign(tinygltf::Camera& c, const Projection& p) {
  c.type = p.type == GltfCameraProjection::Perspective ? "perspective" : "orthographic";
  c.perspective.yfov = p.values[0]; c.perspective.aspectRatio = p.values[1];
  c.perspective.znear = p.values[2]; c.perspective.zfar = p.values[3];
  c.orthographic.xmag = p.values[4]; c.orthographic.ymag = p.values[5];
  c.orthographic.znear = p.values[6]; c.orthographic.zfar = p.values[7];
}
// Canonical decimal IDs and literal camera schema tokens, as in the existing
// KHR_animation_pointer decoder. Scalar /0 components are not supported.
bool cameraPointer(std::string_view path, size_t count, int32_t selected,
    const tinygltf::Camera& camera, std::optional<P>& property) noexcept {
  if (path.empty() || path.front() != '/') return false;
  if (path != "/cameras" && !path.starts_with("/cameras/")) return true;
  if (!path.starts_with("/cameras/")) return false;
  path.remove_prefix(9);
  const auto slash = path.find('/');
  if (slash == std::string_view::npos || slash == 0) return false;
  const auto id = path.substr(0, slash);
  if (id.size() > 1 && id.front() == '0') return false;
  size_t index{};
  for (const char ch : id) {
    if (ch < '0' || ch > '9' || index > (SIZE_MAX - size_t(ch - '0')) / 10) return false;
    index = index * 10 + size_t(ch - '0');
  }
  if (index >= count) return false;
  if (index != size_t(selected)) return true;
  path.remove_prefix(slash + 1);
  for (size_t i = 0; i < paths.size(); ++i) if (path == paths[i]) {
    if ((i < 4) != (camera.type == "perspective")) return false;
    if ((i == 1 && camera.perspective.aspectRatio == 0) || (i == 3 && camera.perspective.zfar == 0)) return false;
    property = P(i); return true;
  }
  return false;
}
struct ScanInfo { uint64_t nodes{}, bindings{}; };
template<class Node, class Binding>
bool scan(GltfCameraAuthoringSourceView source, int32_t selected,
    const GltfCameraAuthoringLimits& limits, Budget* storage, ScanInfo& info,
    Node&& node, Binding&& binding) {
  if (selected < 0 || size_t(selected) >= source.cameras.size() || source.cameras.size() > limits.maxCameraRecords ||
      source.nodes.size() > limits.maxNodes || source.nodes.size() > UINT32_MAX ||
      source.animations.size() > limits.maxAnimationClips) return false;
  for (size_t i = 0; i < source.nodes.size(); ++i) {
    const int camera = source.nodes[i].camera;
    if (camera < -1 || (camera >= 0 && size_t(camera) >= source.cameras.size())) return false;
    if (camera == selected) { ++info.nodes; if (!node(uint32_t(i))) return false; }
  }
  uint64_t channels{}, targetExtensionValues{};
  for (size_t clip = 0; clip < source.animations.size(); ++clip) {
    const auto& animation = source.animations[clip];
    if (animation.channels.size() > limits.maxAnimationChannels - channels) return false;
    channels += animation.channels.size();
    for (size_t i = 0; i < animation.channels.size(); ++i) {
      const auto& channel = animation.channels[i];
      if (channel.target_extensions.size() > limits.maxMetadataValues - targetExtensionValues) return false;
      targetExtensionValues += channel.target_extensions.size();
      // ExtensionMap has a nontransparent std::string comparator. find with a
      // long literal would allocate a temporary string during the final guard.
      const auto ext = std::find_if(channel.target_extensions.begin(), channel.target_extensions.end(),
          [](const auto& item) { return item.first == "KHR_animation_pointer"; });
      if (channel.target_path != "pointer") {
        if (ext != channel.target_extensions.end()) return false;
        continue;
      }
      if (ext == channel.target_extensions.end() || !ext->second.IsObject() || !ext->second.Has("pointer") ||
          !ext->second.Get("pointer").IsString()) return false;
      const auto& path = ext->second.Get("pointer").Get<std::string>();
      if (path.size() > limits.maxPointerBytes) return false;
      std::optional<P> property;
      if (!cameraPointer(path, source.cameras.size(), selected, source.cameras[size_t(selected)], property)) return false;
      if (!property) continue;
      if (channel.target_node != -1 || channel.sampler < 0 || size_t(channel.sampler) >= animation.samplers.size()) return false;
      ++info.bindings;
      if ((storage && !storage->channel(channel)) || !binding(GltfCameraAnimationBinding{clip, i, *property}, channel)) return false;
    }
  }
  return true;
}
struct SourceWitness {
  size_t cameras{}, nodes{}, animations{};
  std::vector<tinygltf::AnimationChannel> channels;
};
bool preflightSource(GltfCameraAuthoringSourceView source, int32_t selected,
    const GltfCameraAuthoringLimits& limits, Budget& budget, ScanInfo& info) noexcept {
  return scan(source, selected, limits, &budget, info, [](uint32_t) { return true; },
      [](const auto&, const auto&) { return true; });
}
bool publicPlanEqual(const GltfCameraProjectionPlan& a, const GltfCameraProjectionPlan& b) noexcept {
  return a.authoredCamera == b.authoredCamera && a.evaluatedCamera == b.evaluatedCamera &&
      exactCamera(a.authoredBefore, b.authoredBefore) && exactCamera(a.authoredAfter, b.authoredAfter) &&
      exactCamera(a.evaluatedBefore, b.evaluatedBefore) && exactCamera(a.evaluatedAfter, b.evaluatedAfter) &&
      a.authoredNodes == b.authoredNodes && a.evaluatedNodes == b.evaluatedNodes &&
      a.authoredAnimationBindings == b.authoredAnimationBindings && a.evaluatedAnimationBindings == b.evaluatedAnimationBindings &&
      a.changedProperties == b.changedProperties && a.changedType == b.changedType &&
      a.changed == b.changed && a.peakOwnedBytes == b.peakOwnedBytes;
}
} // namespace

class GltfCameraAuthoringReceipt {
 public:
  GltfCameraProjectionPlan expected;
  GltfCameraAuthoringLimits limits;
  SourceWitness authored, evaluated;
  GltfCameraProjectionOwnership ownership;
  std::shared_ptr<const GltfCameraAuthoringReceipt> command;
};
struct GltfCameraAuthoringAccess {
  static const auto& receipt(const GltfCameraProjectionPlan& plan) noexcept { return plan.receipt_; }
  static void publish(GltfCameraProjectionPlan& plan, std::shared_ptr<const GltfCameraAuthoringReceipt> receipt) noexcept {
    plan.receipt_ = std::move(receipt);
  }
};
namespace {
bool sourceMatches(GltfCameraAuthoringSourceView source, int32_t selected,
    const tinygltf::Camera& before, std::span<const uint32_t> nodes,
    std::span<const GltfCameraAnimationBinding> bindings, const SourceWitness& witness,
    const GltfCameraAuthoringLimits& limits, Budget& budget, bool compareCamera = true) noexcept {
  if (source.cameras.size() != witness.cameras || source.nodes.size() != witness.nodes ||
      source.animations.size() != witness.animations || selected < 0 || size_t(selected) >= source.cameras.size() ||
      !budget.camera(source.cameras[size_t(selected)]) ||
      (compareCamera && !exactCamera(source.cameras[size_t(selected)], before))) return false;
  size_t n{}, b{}; ScanInfo info;
  const bool valid = scan(source, selected, limits, &budget, info,
      [&](uint32_t index) { return n < nodes.size() && nodes[n++] == index; },
      [&](const auto& binding, const auto& channel) {
        if (b >= bindings.size() || b >= witness.channels.size() || binding != bindings[b] ||
            !exactChannel(channel, witness.channels[b])) return false;
        ++b; return true;
      });
  return valid && n == nodes.size() && b == bindings.size() && b == witness.channels.size();
}
void capture(GltfCameraAuthoringSourceView source, int32_t selected,
    const GltfCameraAuthoringLimits& limits, const ScanInfo& info, std::vector<uint32_t>& nodes,
    std::vector<GltfCameraAnimationBinding>& bindings, SourceWitness& witness) {
  witness.cameras = source.cameras.size(); witness.nodes = source.nodes.size(); witness.animations = source.animations.size();
  nodes.reserve(size_t(info.nodes)); bindings.reserve(size_t(info.bindings)); witness.channels.reserve(size_t(info.bindings));
  ScanInfo ignored;
  if (!scan(source, selected, limits, nullptr, ignored,
      [&](uint32_t index) { nodes.push_back(index); return true; },
      [&](const auto& binding, const auto& channel) { bindings.push_back(binding); witness.channels.push_back(channel); return true; }))
    fail("selected camera context changed during preparation");
}
bool planReceiptMatches(const GltfCameraProjectionPlan& plan) noexcept {
  const auto& receipt = GltfCameraAuthoringAccess::receipt(plan);
  if (!receipt) return false;
  Budget budget{receipt->limits};
  return budget.camera(plan.authoredBefore) && budget.camera(plan.authoredAfter) &&
      budget.camera(plan.evaluatedBefore) && budget.camera(plan.evaluatedAfter) && publicPlanEqual(plan, receipt->expected);
}
GltfCameraProjectionPlan buildCameraProjectionPlan(
    GltfCameraAuthoringSourceView authored, GltfCameraAuthoringSourceView evaluated,
    int32_t authoredCamera, int32_t evaluatedCamera, const Projection& aAfter, const Projection& eAfter,
    GltfCameraProjectionOwnership ownership, const GltfCameraAuthoringLimits& limits,
    std::shared_ptr<const GltfCameraAuthoringReceipt> command = {}) {
  if (!validLimits(limits) || authoredCamera < 0 || evaluatedCamera < 0 ||
      size_t(authoredCamera) >= authored.cameras.size() || size_t(evaluatedCamera) >= evaluated.cameras.size())
    fail("invalid limits or selected camera identity");
  const auto& a = authored.cameras[size_t(authoredCamera)]; const auto& e = evaluated.cameras[size_t(evaluatedCamera)];
  Projection aBefore, eBefore;
  if (!projection(a, aBefore) || !projection(e, eBefore) || !validProjection(aBefore) || !validProjection(eBefore))
    fail("selected camera has an invalid or unrenderable projection");
  if (!validProjection(aAfter)) fail("invalid rebased camera projection");

  Budget budget{limits}; ScanInfo aInfo, eInfo;
  // Includes public and immutable receipt records, vector/control overhead and
  // destination type string growth, before the first owned record allocation.
  if (!budget.add(sizeof(GltfCameraProjectionPlan) + sizeof(GltfCameraAuthoringReceipt) + 1024))
    fail("owned camera storage exceeds its budget");
  if (command && !budget.add(command->expected.peakOwnedBytes)) fail("camera command retention exceeds its budget");
  for (int i = 0; i < 4; ++i) if (!budget.camera(a) || !budget.camera(e))
    fail("camera metadata exceeds its byte, value or depth budget");
  if (!preflightSource(authored, authoredCamera, limits, budget, aInfo) ||
      !preflightSource(evaluated, evaluatedCamera, limits, budget, eInfo))
    fail("invalid or over-budget selected camera/node/animation context");
  if (!budget.array(aInfo.nodes, 2 * sizeof(uint32_t)) || !budget.array(eInfo.nodes, 2 * sizeof(uint32_t)) ||
      !budget.array(aInfo.bindings, 2 * sizeof(GltfCameraAnimationBinding)) ||
      !budget.array(eInfo.bindings, 2 * sizeof(GltfCameraAnimationBinding)) ||
      !budget.array(propertyCount, 2 * sizeof(P))) fail("owned camera context exceeds its budget");

  GltfCameraProjectionPlan plan;
  plan.authoredCamera = authoredCamera; plan.evaluatedCamera = evaluatedCamera;
  plan.authoredBefore = a; plan.authoredAfter = a; plan.evaluatedBefore = e; plan.evaluatedAfter = e;
  assign(plan.authoredAfter, aAfter);
  plan.changedType = aBefore.type != aAfter.type || eBefore.type != eAfter.type;
  auto receipt = std::make_shared<GltfCameraAuthoringReceipt>(); receipt->limits = limits;
  receipt->ownership = ownership; receipt->command = std::move(command);
  capture(authored, authoredCamera, limits, aInfo, plan.authoredNodes, plan.authoredAnimationBindings, receipt->authored);
  capture(evaluated, evaluatedCamera, limits, eInfo, plan.evaluatedNodes, plan.evaluatedAnimationBindings, receipt->evaluated);
  auto prospective = eAfter;
  for (const auto* bindings : {&plan.authoredAnimationBindings, &plan.evaluatedAnimationBindings})
    for (const auto& binding : *bindings) {
      const auto property = size_t(binding.property);
      if (plan.changedType) fail("a camera with animated lens fields cannot change projection type");
      if ((binding.property == P::PerspectiveAspectRatio || binding.property == P::PerspectiveFar) &&
          (ownership.properties & uint16_t(1u << property)) && aAfter.values[property] == 0)
        fail("an animated optional lens field must retain an authored value");
      // The active sample belongs to the player. Rest editing changes its
      // fallback, then the selected transaction evaluates today's clip again.
      prospective.values[property] = eBefore.values[property];
    }
  if (!validProjection(prospective)) fail("invalid evaluated camera projection");
  assign(plan.evaluatedAfter, prospective);
  plan.changedProperties.reserve(propertyCount);
  for (size_t i = 0; i < propertyCount; ++i)
    if (!exact(aBefore.values[i], aAfter.values[i]) || !exact(eBefore.values[i], prospective.values[i])) plan.changedProperties.push_back(P(i));
  plan.changed = plan.changedType || !plan.changedProperties.empty();
  plan.peakOwnedBytes = budget.bytes;
  receipt->expected = plan;
  GltfCameraAuthoringAccess::publish(plan, std::move(receipt));
  return plan;
}
} // namespace

GltfCameraAuthoringSourceView GltfCameraSourceView(const tinygltf::Model& source) noexcept {
  return {source.cameras, source.nodes, source.animations};
}
std::string_view GltfCameraPropertyPath(GltfCameraProperty property) noexcept {
  return size_t(property) < paths.size() ? paths[size_t(property)] : std::string_view{};
}
std::optional<GltfCameraProperty> ParseGltfCameraProperty(std::string_view path) noexcept {
  for (size_t i = 0; i < paths.size(); ++i) if (paths[i] == path) return GltfCameraProperty(i);
  return {};
}
GltfCameraProjectionPlan PlanGltfCameraProjectionEdit(
    GltfCameraAuthoringSourceView authored, GltfCameraAuthoringSourceView evaluated,
    int32_t authoredCamera, int32_t evaluatedCamera, const GltfCameraProjectionPatch& patch,
    const GltfCameraAuthoringLimits& limits) {
  if (!validLimits(limits) || authoredCamera < 0 || evaluatedCamera < 0 ||
      size_t(authoredCamera) >= authored.cameras.size() || size_t(evaluatedCamera) >= evaluated.cameras.size())
    fail("invalid limits or selected camera identity");
  Projection aAfter, eAfter;
  if (!projection(authored.cameras[size_t(authoredCamera)], aAfter) || !validProjection(aAfter) ||
      !projection(evaluated.cameras[size_t(evaluatedCamera)], eAfter) || !validProjection(eAfter))
    fail("selected camera has an invalid or unrenderable projection");
  if (!patchProjection(aAfter, patch, limits) || !patchProjection(eAfter, patch, limits))
    fail("invalid, duplicate, omitted required or incompatible projection edit");
  GltfCameraProjectionOwnership ownership; ownership.type = patch.type.has_value();
  for (const auto& edit : patch.values) ownership.properties |= uint16_t(1u << size_t(edit.property));
  return buildCameraProjectionPlan(authored, evaluated, authoredCamera, evaluatedCamera, aAfter, eAfter, ownership, limits);
}
bool GltfCameraProjectionEditMatches(const GltfCameraProjectionPlan& plan,
    GltfCameraAuthoringSourceView authored, GltfCameraAuthoringSourceView evaluated) noexcept {
  const auto& receipt = GltfCameraAuthoringAccess::receipt(plan);
  if (!receipt) return false;
  Budget budget{receipt->limits};
  // Public mutable fields are never a substitute for the private receipt.
  // Bound possibly changed metadata before recursive bit-exact comparisons.
  if (!budget.camera(plan.authoredBefore) || !budget.camera(plan.authoredAfter) ||
      !budget.camera(plan.evaluatedBefore) || !budget.camera(plan.evaluatedAfter) ||
      !publicPlanEqual(plan, receipt->expected)) return false;
  return sourceMatches(authored, plan.authoredCamera, plan.authoredBefore, plan.authoredNodes,
      plan.authoredAnimationBindings, receipt->authored, receipt->limits, budget) &&
      sourceMatches(evaluated, plan.evaluatedCamera, plan.evaluatedBefore, plan.evaluatedNodes,
      plan.evaluatedAnimationBindings, receipt->evaluated, receipt->limits, budget);
}
void ValidateGltfCameraProjectionEditBefore(const GltfCameraProjectionPlan& plan,
    GltfCameraAuthoringSourceView authored, GltfCameraAuthoringSourceView evaluated) {
  if (!GltfCameraProjectionEditMatches(plan, authored, evaluated)) fail("stale or modified camera projection receipt");
}
uint64_t GltfCameraRecordStorageBytes(const tinygltf::Camera& camera, const GltfCameraAuthoringLimits& limits) {
  if (!validLimits(limits)) fail("invalid selected camera storage limits");
  Budget budget{limits};
  if (!budget.camera(camera)) fail("selected camera metadata exceeds its budget");
  return budget.bytes;
}
bool GltfCameraRecordsEqual(const tinygltf::Camera& a, const tinygltf::Camera& b) noexcept { return exactCamera(a, b); }
bool GltfCameraProjectionValid(const tinygltf::Camera& camera) noexcept {
  Projection p; return projection(camera, p) && validProjection(p);
}
GltfCameraProjectionOwnership GltfCameraProjectionOwnedFields(const GltfCameraProjectionPlan& plan) {
  if (!planReceiptMatches(plan)) fail("modified camera projection receipt");
  return GltfCameraAuthoringAccess::receipt(plan)->ownership;
}
GltfCameraProjectionPlan RebaseGltfCameraProjectionEdit(const GltfCameraProjectionPlan& original,
    GltfCameraEditSide side, GltfCameraAuthoringSourceView authored, GltfCameraAuthoringSourceView evaluated,
    const GltfCameraAuthoringLimits& limits) {
  if ((side != GltfCameraEditSide::Before && side != GltfCameraEditSide::After) || !planReceiptMatches(original))
    fail("invalid camera replay endpoint or modified command");
  const auto& receipt = GltfCameraAuthoringAccess::receipt(original);
  const auto root = receipt->command ? receipt->command : receipt;
  const auto& trusted = root->expected;
  Budget context{limits};
  if (!validLimits(limits) || !sourceMatches(authored, trusted.authoredCamera, trusted.authoredBefore,
      trusted.authoredNodes, trusted.authoredAnimationBindings, root->authored, limits, context, false) ||
      !sourceMatches(evaluated, trusted.evaluatedCamera, trusted.evaluatedBefore,
      trusted.evaluatedNodes, trusted.evaluatedAnimationBindings, root->evaluated, limits, context, false))
    fail("camera replay context or shared definition uses changed");
  const auto& a = authored.cameras[size_t(trusted.authoredCamera)];
  const auto& e = evaluated.cameras[size_t(trusted.evaluatedCamera)];
  const auto& expectedA = side == GltfCameraEditSide::Before ? trusted.authoredAfter : trusted.authoredBefore;
  const auto& expectedE = side == GltfCameraEditSide::Before ? trusted.evaluatedAfter : trusted.evaluatedBefore;
  const auto& desiredA = side == GltfCameraEditSide::Before ? trusted.authoredBefore : trusted.authoredAfter;
  const auto& desiredE = side == GltfCameraEditSide::Before ? trusted.evaluatedBefore : trusted.evaluatedAfter;
  Projection nextA, nextE; projection(a, nextA); projection(e, nextE);
  const auto oldA = scalars(expectedA), oldE = scalars(expectedE), wantedA = scalars(desiredA), wantedE = scalars(desiredE);
  if (a.type != expectedA.type || e.type != expectedE.type) fail("camera replay projection type changed");
  uint16_t animated{};
  for (const auto* bindings : {&trusted.authoredAnimationBindings, &trusted.evaluatedAnimationBindings})
    for (const auto& binding : *bindings) animated |= uint16_t(1u << size_t(binding.property));
  for (size_t i = 0; i < propertyCount; ++i) if (root->ownership.properties & uint16_t(1u << i)) {
    if (!exact(nextA.values[i], oldA[i]) ||
        (!(animated & uint16_t(1u << i)) && !exact(nextE.values[i], oldE[i])))
      fail("camera replay owned projection leaf changed");
    nextA.values[i] = wantedA[i];
    if (!(animated & uint16_t(1u << i))) nextE.values[i] = wantedE[i];
  }
  if (root->ownership.type) {
    nextA.type = desiredA.type == "perspective" ? GltfCameraProjection::Perspective : GltfCameraProjection::Orthographic;
    nextE.type = desiredE.type == "perspective" ? GltfCameraProjection::Perspective : GltfCameraProjection::Orthographic;
  }
  return buildCameraProjectionPlan(authored, evaluated, trusted.authoredCamera, trusted.evaluatedCamera,
      nextA, nextE, root->ownership, limits, root);
}
} // namespace container::geometry::gltf
