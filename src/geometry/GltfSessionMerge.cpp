#include <Container/geometry/GltfSessionMerge.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace container::geometry::gltf {
namespace {
using Value = tinygltf::Value;
using Object = Value::Object;

[[noreturn]] void fail(std::string_view detail) {
  throw std::runtime_error("glTF session merge: " + std::string(detail));
}
uint64_t add(uint64_t a, uint64_t b) {
  if (b > UINT64_MAX - a) fail("budget integer overflow");
  return a + b;
}
uint64_t multiply(uint64_t a, uint64_t b) {
  if (a && b > UINT64_MAX / a) fail("budget integer overflow");
  return a * b;
}

// Include the complete live metadata, rather than assuming that an evaluated
// vector/default leaf occupies no more space than its authored representation.
struct MetadataCost {
  const GltfMergeLimits& limits;
  uint64_t bytes{};
  void charge(uint64_t size) {
    bytes = add(bytes, size);
    if (bytes > limits.maxMetadataBytes) fail("metadata budget exceeded");
  }
  void text(std::string_view s) { charge(add(s.size(), 16)); }
  void number(double v) {
    if (!std::isfinite(v)) fail("nonfinite evaluated metadata");
    charge(16);
  }
  template<class T> void numbers(const std::vector<T>& values) {
    charge(multiply(values.size(), 16));
    for (const auto v : values)
      if constexpr (std::is_floating_point_v<T>)
        if (!std::isfinite(v)) fail("nonfinite evaluated vector");
  }
  void value(const Value& v, uint32_t depth = 0) {
    if (depth > limits.maxMetadataDepth) fail("metadata depth exceeded");
    charge(32);
    if (v.IsString()) text(v.Get<std::string>());
    else if (v.IsNumber()) number(v.GetNumberAsDouble());
    else if (v.IsBinary()) charge(v.Get<std::vector<unsigned char>>().size());
    else if (v.IsArray()) for (const auto& child : v.Get<Value::Array>()) value(child, depth + 1);
    else if (v.IsObject()) for (const auto& [key, child] : v.Get<Object>()) {
      text(key); value(child, depth + 1);
    }
  }
  void extensions(const tinygltf::ExtensionMap& values) {
    for (const auto& [name, v] : values) { text(name); value(v); }
  }
  template<class T> void object(const T& v) {
    charge(128);
    if constexpr (requires { v.name; }) text(v.name);
    if constexpr (requires { v.extras; }) value(v.extras);
    if constexpr (requires { v.extensions; }) extensions(v.extensions);
    if constexpr (requires { v.extras_json_string; }) text(v.extras_json_string);
    if constexpr (requires { v.extensions_json_string; }) text(v.extensions_json_string);
#define SESSION_TEXT(member) if constexpr (requires { std::string_view(v.member); }) text(v.member)
    SESSION_TEXT(uri); SESSION_TEXT(mimeType); SESSION_TEXT(type); SESSION_TEXT(alphaMode);
    SESSION_TEXT(version); SESSION_TEXT(minVersion); SESSION_TEXT(generator); SESSION_TEXT(copyright);
    SESSION_TEXT(interpolation); SESSION_TEXT(target_path);
#undef SESSION_TEXT
#define SESSION_NUMBERS(member) if constexpr (requires { v.member; }) numbers(v.member)
    SESSION_NUMBERS(children); SESSION_NUMBERS(lods); SESSION_NUMBERS(rotation);
    SESSION_NUMBERS(translation); SESSION_NUMBERS(matrix); SESSION_NUMBERS(weights);
    SESSION_NUMBERS(joints); SESSION_NUMBERS(minValues); SESSION_NUMBERS(maxValues);
    SESSION_NUMBERS(emissiveFactor); SESSION_NUMBERS(baseColorFactor); SESSION_NUMBERS(color);
#undef SESSION_NUMBERS
    if constexpr (requires { v.scale; }) {
      if constexpr (std::is_arithmetic_v<std::remove_cvref_t<decltype(v.scale)>>) number(v.scale);
      else numbers(v.scale);
    }
#define SESSION_NUMBER(member) if constexpr (requires { v.member; }) number(v.member)
    SESSION_NUMBER(metallicFactor); SESSION_NUMBER(roughnessFactor); SESSION_NUMBER(alphaCutoff);
    SESSION_NUMBER(strength); SESSION_NUMBER(intensity); SESSION_NUMBER(range);
    SESSION_NUMBER(innerConeAngle); SESSION_NUMBER(outerConeAngle);
    SESSION_NUMBER(yfov); SESSION_NUMBER(aspectRatio); SESSION_NUMBER(znear); SESSION_NUMBER(zfar);
    SESSION_NUMBER(xmag); SESSION_NUMBER(ymag);
#undef SESSION_NUMBER
    if constexpr (requires { v.pbrMetallicRoughness; }) {
      object(v.pbrMetallicRoughness); object(v.normalTexture); object(v.occlusionTexture); object(v.emissiveTexture);
      for (const auto* map : {&v.values, &v.additionalValues}) for (const auto& [key, p] : *map) {
        text(key); text(p.string_value); numbers(p.number_array); number(p.number_value);
        for (const auto& [name, n] : p.json_double_value) { text(name); number(n); }
      }
    }
    if constexpr (requires { v.baseColorTexture; }) { object(v.baseColorTexture); object(v.metallicRoughnessTexture); }
    if constexpr (requires { v.spot; }) object(v.spot);
    if constexpr (requires { v.perspective; }) { object(v.perspective); object(v.orthographic); }
    if constexpr (requires { v.attributes; }) for (const auto& [name, _] : v.attributes) { text(name); charge(16); }
    if constexpr (requires { v.targets; }) for (const auto& target : v.targets)
      for (const auto& [name, _] : target) { text(name); charge(16); }
    if constexpr (requires { v.primitives; }) for (const auto& p : v.primitives) object(p);
    if constexpr (requires { v.samplers; }) for (const auto& s : v.samplers) object(s);
    if constexpr (requires { v.channels; }) for (const auto& c : v.channels) object(c);
    if constexpr (requires { v.target_extensions; }) extensions(v.target_extensions);
    if constexpr (requires { v.target_extras; }) value(v.target_extras);
    if constexpr (requires { v.target_extras_json_string; }) text(v.target_extras_json_string);
    if constexpr (requires { v.target_extensions_json_string; }) text(v.target_extensions_json_string);
    if constexpr (requires { v.sparse; }) {
      // Sparse's metadata includes three independently extensible objects.
      object(v.sparse); object(v.sparse.indices); object(v.sparse.values);
    }
  }
};
uint64_t metadataCost(const tinygltf::Model& m, const GltfMergeLimits& limits) {
  MetadataCost c{limits}; c.object(m.asset); c.value(m.extras); c.extensions(m.extensions);
  c.text(m.extras_json_string); c.text(m.extensions_json_string);
  for (const auto* names : {&m.extensionsUsed, &m.extensionsRequired}) for (const auto& n : *names) c.text(n);
#define SESSION_TABLE(member) for (const auto& v : m.member) c.object(v)
  SESSION_TABLE(buffers); SESSION_TABLE(bufferViews); SESSION_TABLE(accessors);
  SESSION_TABLE(images); SESSION_TABLE(samplers); SESSION_TABLE(textures);
  SESSION_TABLE(materials); SESSION_TABLE(meshes); SESSION_TABLE(nodes); SESSION_TABLE(skins);
  SESSION_TABLE(cameras); SESSION_TABLE(lights); SESSION_TABLE(animations); SESSION_TABLE(scenes);
#undef SESSION_TABLE
  for (const auto& scene : m.scenes) c.numbers(scene.nodes);
  for (const auto& scene : m.scenes) c.numbers(scene.audioEmitters);
  if (!m.audioEmitters.empty() || !m.audioSources.empty()) fail("audio identities are unsupported");
  return c.bytes;
}
uint64_t residentBytes(const tinygltf::Model& m) {
  uint64_t bytes{};
  for (const auto& b : m.buffers) bytes = add(bytes, b.data.size());
  for (const auto& i : m.images) bytes = add(bytes, i.image.size());
  return bytes;
}
template<class T> void sameTable(const std::vector<T>& live, const std::vector<T>& archive,
                                 const char* label, bool suffix = false) {
  if ((suffix ? live.size() > archive.size() : live.size() != archive.size()) ||
      !std::equal(live.begin(), live.end(), archive.begin())) fail(label);
}
template<class T> bool sameProperties(const T& a, const T& b) {
  return a.extras == b.extras && a.extensions == b.extensions &&
      a.extras_json_string == b.extras_json_string && a.extensions_json_string == b.extensions_json_string;
}
bool numeric(const Value& v) {
  if (v.IsNumber()) return true;
  return v.IsArray() && !v.Get<Value::Array>().empty() &&
      std::ranges::all_of(v.Get<Value::Array>(), [](const auto& n) { return n.IsNumber(); });
}
// Only numeric material leaves can change. Index/UV bindings and opaque extras
// remain exact. This permits authored default leaves to become explicit values.
bool sameMaterialValue(const Value& a, const Value& b, uint32_t depth, uint32_t maxDepth) {
  if (depth > maxDepth) fail("material metadata depth exceeded");
  if (a.IsNumber() && b.IsNumber()) return true;
  if (a.IsArray() && b.IsArray()) {
    const auto& x = a.Get<Value::Array>(); const auto& y = b.Get<Value::Array>();
    if (x.size() != y.size()) return false;
    for (size_t i = 0; i < x.size(); ++i) if (!sameMaterialValue(x[i], y[i], depth + 1, maxDepth)) return false;
    return true;
  }
  if (a.IsObject() && b.IsObject()) {
    const auto& x = a.Get<Object>(); const auto& y = b.Get<Object>();
    for (const auto& [key, v] : x) {
      const auto found = y.find(key);
      if (found == y.end()) { if (!numeric(v) || key == "extras" || key == "index" || key == "texCoord") return false; }
      else if (key == "extras" || key == "index" || key == "texCoord") { if (!(v == found->second)) return false; }
      else if (!sameMaterialValue(v, found->second, depth + 1, maxDepth)) return false;
    }
    for (const auto& [key, v] : y) if (!x.contains(key))
      if (!numeric(v) || key == "extras" || key == "index" || key == "texCoord") return false;
    return true;
  }
  return a == b;
}
bool sameMaterialExtensions(const tinygltf::ExtensionMap& a, const tinygltf::ExtensionMap& b, uint32_t depth) {
  if (a.size() != b.size()) return false;
  for (const auto& [name, v] : a) {
    const auto found = b.find(name);
    if (found == b.end() || !sameMaterialValue(v, found->second, 0, depth)) return false;
  }
  return true;
}
bool sameRootExtensions(const tinygltf::ExtensionMap& a, const tinygltf::ExtensionMap& b, uint32_t depth) {
  if (a.size()!=b.size()) return false;
  for (const auto& [name,v]:a) {
    const auto found=b.find(name);
    if (found==b.end()) return false;
    if (name=="KHR_lights_punctual") {
      if (!sameMaterialValue(v,found->second,0,depth)) return false;
    } else if (!(v==found->second)) return false;
  }
  return true;
}
template<class T> bool sameTextureInfo(const T& a, const T& b, uint32_t depth) {
  return a.index == b.index && a.texCoord == b.texCoord && a.extras == b.extras &&
      a.extras_json_string == b.extras_json_string && a.extensions_json_string == b.extensions_json_string &&
      sameMaterialExtensions(a.extensions, b.extensions, depth);
}
void validatePrefix(const tinygltf::Model& live, const tinygltf::Model& archive, const GltfMergeLimits& limits) {
  sameTable(live.accessors, archive.accessors, "accessor identities changed", true);
  sameTable(live.bufferViews, archive.bufferViews, "buffer-view identities changed", true);
  sameTable(live.samplers, archive.samplers, "sampler identities changed", true);
  sameTable(live.textures, archive.textures, "texture bindings changed", true);
  sameTable(live.meshes, archive.meshes, "mesh identities changed");
  sameTable(live.skins, archive.skins, "skin identities changed");
  sameTable(live.animations, archive.animations, "clip identities changed");
  sameTable(live.scenes, archive.scenes, "scene identities changed");
  // TinyGLTF equality omits sparse references, primitive extensions and
  // channel target_extensions. Those omissions are identity-bearing here.
  for (size_t i=0;i<live.accessors.size();++i) {
    const auto& a=live.accessors[i]; const auto& b=archive.accessors[i];
    if (!sameProperties(a,b) || a.minValues!=b.minValues || a.maxValues!=b.maxValues || a.sparse.isSparse!=b.sparse.isSparse)
      fail("accessor metadata or sparse identity changed");
    if (a.sparse.isSparse) {
      const auto& x=a.sparse; const auto& y=b.sparse;
      if (x.count!=y.count || x.indices.bufferView!=y.indices.bufferView || x.indices.byteOffset!=y.indices.byteOffset ||
          x.indices.componentType!=y.indices.componentType || x.values.bufferView!=y.values.bufferView ||
          x.values.byteOffset!=y.values.byteOffset || !sameProperties(x,y) || !sameProperties(x.indices,y.indices) ||
          !sameProperties(x.values,y.values)) fail("sparse accessor references changed");
    }
  }
  for (size_t i=0;i<live.bufferViews.size();++i)
    if (!sameProperties(live.bufferViews[i],archive.bufferViews[i])) fail("buffer-view metadata changed");
  for (size_t i=0;i<live.samplers.size();++i)
    if (!sameProperties(live.samplers[i],archive.samplers[i])) fail("sampler metadata changed");
  for (size_t i=0;i<live.textures.size();++i)
    if (!sameProperties(live.textures[i],archive.textures[i])) fail("texture metadata changed");
  for (size_t i=0;i<live.meshes.size();++i) {
    const auto& a=live.meshes[i]; const auto& b=archive.meshes[i];
    if (!sameProperties(a,b) || a.weights!=b.weights) fail("mesh metadata changed");
    for (size_t p=0;p<a.primitives.size();++p)
      if (!sameProperties(a.primitives[p],b.primitives[p])) fail("primitive extension identity changed");
  }
  for (size_t i=0;i<live.skins.size();++i)
    if (!sameProperties(live.skins[i],archive.skins[i])) fail("skin metadata changed");
  for (size_t i=0;i<live.scenes.size();++i)
    if (!sameProperties(live.scenes[i],archive.scenes[i]) || live.scenes[i].audioEmitters!=archive.scenes[i].audioEmitters)
      fail("scene metadata changed");
  for (size_t i=0;i<live.animations.size();++i) {
    const auto& a=live.animations[i]; const auto& b=archive.animations[i];
    if (!sameProperties(a,b)) fail("clip metadata changed");
    for (size_t s=0;s<a.samplers.size();++s)
      if (!sameProperties(a.samplers[s],b.samplers[s])) fail("animation sampler identity changed");
    for (size_t c=0;c<a.channels.size();++c)
      if (!sameProperties(a.channels[c],b.channels[c]) || a.channels[c].target_extensions!=b.channels[c].target_extensions ||
          a.channels[c].target_extras!=b.channels[c].target_extras ||
          a.channels[c].target_extras_json_string!=b.channels[c].target_extras_json_string ||
          a.channels[c].target_extensions_json_string!=b.channels[c].target_extensions_json_string)
        fail("animation target identity changed");
  }
  if (live.buffers.size() > archive.buffers.size() || live.images.size() > archive.images.size())
    fail("archive removed resource identities");
  for (size_t i = 0; i < live.buffers.size(); ++i)
    if (live.buffers[i].data != archive.buffers[i].data || live.buffers[i].name != archive.buffers[i].name ||
        !sameProperties(live.buffers[i], archive.buffers[i])) fail("resident buffer prefix changed");
  for (const auto& buffer : archive.buffers)
    if (buffer.data.empty() && !buffer.uri.empty()) fail("archive buffer is not resident");
  for (size_t i = 0; i < live.images.size(); ++i) {
    const auto& a=live.images[i]; const auto& b=archive.images[i];
    if (a.name != b.name || !sameProperties(a,b))
      fail("image provenance metadata changed");
    const bool comparable=a.as_is==b.as_is && a.mimeType==b.mimeType && a.width==b.width &&
        a.height==b.height && a.component==b.component && a.bits==b.bits && a.pixel_type==b.pixel_type;
    if (comparable && !a.image.empty() && !b.image.empty() && a.image!=b.image)
      fail("image payload changed within the same representation");
  }
  if (live.nodes.size() != archive.nodes.size() || live.materials.size() != archive.materials.size() ||
      live.lights.size() != archive.lights.size() || live.cameras.size() != archive.cameras.size())
    fail("authored object identities changed");
  if (!(live.asset == archive.asset) || !sameProperties(live.asset,archive.asset) ||
      !(live.extras == archive.extras) || live.extras_json_string!=archive.extras_json_string ||
      live.extensions_json_string!=archive.extensions_json_string ||
      !sameRootExtensions(live.extensions,archive.extensions,limits.maxMetadataDepth))
    fail("source root identity changed");
  for (const auto& name : live.extensionsUsed)
    if (std::ranges::find(archive.extensionsUsed, name) == archive.extensionsUsed.end()) fail("archive lost an extension declaration");
  for (const auto& name : live.extensionsRequired)
    if (std::ranges::find(archive.extensionsRequired, name) == archive.extensionsRequired.end()) fail("archive lost a required extension");
  for (size_t i = 0; i < live.nodes.size(); ++i) {
    const auto& a = live.nodes[i]; const auto& b = archive.nodes[i];
    if (a.name != b.name || a.mesh != b.mesh || a.skin != b.skin || a.camera != b.camera || a.light != b.light ||
        a.emitter != b.emitter || a.children != b.children || a.lods != b.lods || a.matrix != b.matrix || !sameProperties(a,b))
      fail("node binding identity changed");
    if ((!a.translation.empty() && a.translation.size() != 3) || (!a.rotation.empty() && a.rotation.size() != 4) ||
        (!a.scale.empty() && a.scale.size() != 3)) fail("evaluated node pose has invalid shape");
  }
  for (size_t i = 0; i < live.materials.size(); ++i) {
    const auto& a = live.materials[i]; const auto& b = archive.materials[i];
    const auto& x = a.pbrMetallicRoughness; const auto& y = b.pbrMetallicRoughness;
    if (x.baseColorFactor.size()!=4 || a.emissiveFactor.size()!=3) fail("evaluated material vector has invalid shape");
    if (a.name != b.name || a.alphaMode != b.alphaMode || a.doubleSided != b.doubleSided || a.lods != b.lods ||
        !(a.extras == b.extras) || a.extras_json_string != b.extras_json_string || a.extensions_json_string != b.extensions_json_string ||
        a.values != b.values || a.additionalValues != b.additionalValues || !sameProperties(x,y) ||
        !sameMaterialExtensions(a.extensions,b.extensions,limits.maxMetadataDepth) ||
        !sameTextureInfo(x.baseColorTexture,y.baseColorTexture,limits.maxMetadataDepth) ||
        !sameTextureInfo(x.metallicRoughnessTexture,y.metallicRoughnessTexture,limits.maxMetadataDepth) ||
        !sameTextureInfo(a.normalTexture,b.normalTexture,limits.maxMetadataDepth) ||
        !sameTextureInfo(a.occlusionTexture,b.occlusionTexture,limits.maxMetadataDepth) ||
        !sameTextureInfo(a.emissiveTexture,b.emissiveTexture,limits.maxMetadataDepth)) fail("material binding identity changed");
  }
  for (size_t i = 0; i < live.lights.size(); ++i) {
    const auto& a = live.lights[i]; const auto& b = archive.lights[i];
    if (!a.color.empty() && a.color.size()!=3) fail("evaluated light color has invalid shape");
    if (a.name != b.name || a.type != b.type || !sameProperties(a,b) || !sameProperties(a.spot,b.spot)) fail("light identity changed");
  }
  for (size_t i = 0; i < live.cameras.size(); ++i) {
    const auto& a = live.cameras[i]; const auto& b = archive.cameras[i];
    if (a.name != b.name || a.type != b.type || !sameProperties(a,b) ||
        !sameProperties(a.perspective,b.perspective) || !sameProperties(a.orthographic,b.orthographic)) fail("camera identity changed");
  }
  if (live.defaultScene < -1 || (live.defaultScene >= 0 && size_t(live.defaultScene) >= live.scenes.size()))
    fail("live selected scene is invalid");
}
bool authoringEmpty(const tinygltf::Model& m) {
  return m.materials.empty() && m.meshes.empty() && m.nodes.empty() &&
      m.skins.empty() && m.cameras.empty() && m.lights.empty() && m.animations.empty() && m.scenes.empty() &&
      m.extensions.empty() && m.extras == Value{} && m.asset.extras == Value{} &&
      m.asset.extensions.empty() && m.extras_json_string.empty() && m.extensions_json_string.empty() &&
      m.asset.extras_json_string.empty() && m.asset.extensions_json_string.empty() &&
      m.asset.minVersion.empty() && m.asset.generator.empty() && m.asset.copyright.empty();
}
} // namespace

GltfSessionMergePlan PlanGltfSessionSources(const tinygltf::Model& live, const tinygltf::Model& archive,
    const tinygltf::Model& incoming, const GltfMergeOptions& options, const GltfMergeLimits& limits) {
  const auto liveMetadata = metadataCost(live,limits);
  const auto archiveMetadata = metadataCost(archive,limits); // Bound recursive equality before comparing prefixes.
  const auto payload = add(residentBytes(archive),residentBytes(incoming));
  const auto peak = add(residentBytes(live),multiply(payload,3));
  if (peak > limits.maxResidentBytes) fail("session inputs and both output payloads exceed budget");
  if (liveMetadata >= limits.maxMetadataBytes) fail("session metadata budget exhausted");
  validatePrefix(live,archive,limits);
  GltfMergeLimits plannedLimits = limits;
  plannedLimits.maxMetadataBytes = (limits.maxMetadataBytes-liveMetadata)/2;
  if (options.parentName.size()>plannedLimits.maxMetadataBytes) fail("placement name exceeds metadata budget");
  GltfMergeOptions selected = options;
  // The live catalog uses scene zero when its optional authored default is
  // absent. Do not fall back to a different default retained in the archive.
  if (!selected.destinationScene) selected.destinationScene = live.defaultScene >= 0 ? size_t(live.defaultScene) : 0;
  GltfSessionMergePlan result;
  result.merge = [&] {
    if (archive.asset.version.empty() && authoringEmpty(archive)) {
      // This temporary is gone before copying the evaluated output. Its payload
      // fits the same peak: archive <= archive+incoming.
      auto normalized = archive; normalized.asset.version = "2.0";
      return PlanGltfMerge(normalized,incoming,selected,plannedLimits);
    }
    return PlanGltfMerge(archive,incoming,selected,plannedLimits);
  }();
  result.peakResidentBytes = peak;
  // Recount the complete result before the second model copy. Taking the larger
  // estimate also covers typed fields/raw JSON not represented by the planner's
  // serialized estimate. The live overlay is reserved separately, since an
  // evaluated default leaf can be larger than its authored counterpart.
  const auto outputMetadata = std::max(result.merge.summary.metadataBytes,metadataCost(result.merge.model,limits));
  result.metadataBytes = std::max(archiveMetadata,add(liveMetadata,multiply(outputMetadata,2)));
  if (result.metadataBytes > limits.maxMetadataBytes) fail("session output metadata exceeds budget");
  result.evaluatedSource = result.merge.model;
  std::copy(live.nodes.begin(),live.nodes.end(),result.evaluatedSource.nodes.begin());
  std::copy(live.materials.begin(),live.materials.end(),result.evaluatedSource.materials.begin());
  std::copy(live.lights.begin(),live.lights.end(),result.evaluatedSource.lights.begin());
  std::copy(live.cameras.begin(),live.cameras.end(),result.evaluatedSource.cameras.begin());
  result.evaluatedSource.defaultScene = live.defaultScene;
  return result;
}
} // namespace container::geometry::gltf
