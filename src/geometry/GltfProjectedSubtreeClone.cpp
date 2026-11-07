#include <Container/geometry/GltfProjectedSubtreeClone.h>
#include <Container/geometry/GltfAccessor.h>
#include <Container/geometry/GltfAnimation.h>
#include <Container/geometry/GltfInstancing.h>
#include <Container/geometry/GltfNodeFlags.h>
#include <Container/geometry/GltfSceneCatalog.h>
#include <Container/utility/SceneNodeAuthoringPolicy.h>

#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <climits>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace container::geometry::gltf {
namespace {
// Keep the source-pair identity contract aligned with GltfSessionMerge. This
// detached planner cannot infer an image's decoded/encoded origin from bytes.
using Value = tinygltf::Value;
using Object = Value::Object;

[[noreturn]] void fail(std::string_view detail) {
  throw std::runtime_error("glTF projected subtree clone: " + std::string(detail));
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

void charge(uint64_t& total, uint64_t amount, uint64_t limit, const char* what) {
  total = add(total, amount);
  if (total > limit) fail(what);
}
void index(int value, size_t count, const char* what, bool optional = false) {
  if ((optional && value == -1) || (value >= 0 && size_t(value) < count)) return;
  fail(what);
}
const Value& field(const Value& object, const char* key) {
  if (!object.IsObject() || !object.Has(key)) fail("extension field is missing");
  return object.Get(key);
}
int integer(const Value& value) {
  if (!value.IsInt() || value.Get<int>() < 0) fail("extension index is not a nonnegative integer");
  return value.Get<int>();
}
bool declared(const tinygltf::Model& model, const std::string& name) {
  return std::ranges::find(model.extensionsUsed, name) != model.extensionsUsed.end();
}
bool area(std::string_view name) {
  return name == "KHR_lights_area" || name == "EXT_lights_area" ||
      name == "WEBGI_lights_area" || name == "WEBGI_area_light";
}
bool nodeExtension(std::string_view name) {
  return area(name) || name == "KHR_lights_punctual" || name == "EXT_mesh_gpu_instancing" ||
      name == "KHR_node_visibility" || name == "KHR_node_selectability" || name == "KHR_node_hoverability";
}
bool visualExtension(std::string_view name) {
  return area(name) || name == "KHR_lights_punctual" || name == "EXT_mesh_gpu_instancing";
}
bool exactValue(const Value& a,const Value& b) {
  std::vector<std::pair<const Value*,const Value*>> pending{{&a,&b}};
  while(!pending.empty()) {
    const auto [left,right]=pending.back();pending.pop_back();
    if(left->Type()!=right->Type()) return false;
    if(left->IsInt() && left->Get<int>()!=right->Get<int>()) return false;
    if(left->IsReal() && std::bit_cast<uint64_t>(left->Get<double>())!=std::bit_cast<uint64_t>(right->Get<double>())) return false;
    if(left->IsBool() && left->Get<bool>()!=right->Get<bool>()) return false;
    if(left->IsString() && left->Get<std::string>()!=right->Get<std::string>()) return false;
    if(left->IsBinary() && left->Get<std::vector<unsigned char>>()!=right->Get<std::vector<unsigned char>>()) return false;
    if(left->IsArray()) {
      const auto& x=left->Get<Value::Array>();const auto& y=right->Get<Value::Array>();
      if(x.size()!=y.size()) return false;
      for(size_t n=0;n<x.size();++n) pending.push_back({&x[n],&y[n]});
    } else if(left->IsObject()) {
      const auto& x=left->Get<Object>();const auto& y=right->Get<Object>();if(x.size()!=y.size()) return false;
      auto next=y.begin();for(const auto& [key,value]:x) {
        if(key!=next->first) return false;pending.push_back({&value,&next->second});++next;
      }
    }
  }
  return true;
}
bool exactExtensions(const tinygltf::ExtensionMap& a,const tinygltf::ExtensionMap& b) {
  if(a.size()!=b.size()) return false;auto next=b.begin();
  for(const auto& [key,value]:a) {if(key!=next->first || !exactValue(value,next->second)) return false;++next;}
  return true;
}
template<class T> bool exactMetadata(const T& a,const T& b) {
  return exactValue(a.extras,b.extras) && exactExtensions(a.extensions,b.extensions) &&
      a.extras_json_string==b.extras_json_string && a.extensions_json_string==b.extensions_json_string;
}
void validateTransform(const tinygltf::Node& node) {
  const auto vector = [](const std::vector<double>& values, size_t count) {
    if (!values.empty() && values.size() != count) fail("node transform has invalid shape");
    for (const auto v : values) if (!std::isfinite(v)) fail("nonfinite node transform");
  };
  vector(node.matrix, 16); vector(node.translation, 3);
  vector(node.rotation, 4); vector(node.scale, 3);
  for (const auto v : node.weights) if (!std::isfinite(v)) fail("nonfinite morph weight");
  if (!node.matrix.empty()) {
    if (!node.translation.empty() || !node.rotation.empty() || !node.scale.empty())
      fail("node has both matrix and TRS transforms");
    if (node.matrix[3] != 0 || node.matrix[7] != 0 || node.matrix[11] != 0 || node.matrix[15] != 1)
      fail("node matrix is not affine");
  }
  if (!node.rotation.empty()) {
    double norm{};
    for (const auto v : node.rotation) norm += v * v;
    if (!std::isfinite(norm) || std::abs(norm - 1) > 1e-5)
      fail("node quaternion is not unit length");
  }
}
void noExtensions(const tinygltf::ExtensionMap& extensions, const char* what) {
  if (!extensions.empty()) fail(what);
}
void validateCloneNode(const tinygltf::Model& model, const tinygltf::Node& node, bool validateMesh) {
  validateTransform(node);
  index(node.mesh, model.meshes.size(), "node mesh is invalid", true);
  index(node.skin, model.skins.size(), "node skin is invalid", true);
  index(node.camera, model.cameras.size(), "node camera is invalid", true);
  index(node.light, model.lights.size(), "node light is invalid", true);
  if (node.emitter != -1 || !node.lods.empty()) fail("audio/LOD node references are unsupported");
  for (const auto& [name, value] : node.extensions) {
    if (!nodeExtension(name) || !declared(model, name)) fail("unsupported or undeclared node extension");
    if (!value.IsObject()) fail("node extension is not an object");
    if (name == "KHR_lights_punctual") {
      const auto light = integer(field(value, "light"));
      index(light, model.lights.size(), "punctual light reference is invalid");
      if (node.light != -1 && node.light != light) fail("typed and extension light references disagree");
    } else if (area(name)) {
      const auto root = model.extensions.find(name);
      if (root == model.extensions.end()) fail("area light root table is absent");
      const auto& lights = field(root->second, "lights");
      if (!lights.IsArray()) fail("area light table is not an array");
      index(integer(field(value, "light")), lights.Get<Value::Array>().size(), "area light reference is invalid");
    } else if (name == "EXT_mesh_gpu_instancing") {
      const auto& attributes = field(value, "attributes");
      if (node.mesh == -1 || !attributes.IsObject() || attributes.Get<Object>().empty())
        fail("instancing has no mesh or attribute map");
      for (const auto& [_, accessor] : attributes.Get<Object>())
        index(integer(accessor), model.accessors.size(), "instance accessor is invalid");
    }
  }
  if (node.skin >= 0 && node.mesh < 0) fail("skin is attached to a node without a mesh");
  if (node.mesh >= 0) {
    const auto& mesh = model.meshes[size_t(node.mesh)];
    if (mesh.primitives.empty()) fail("selected mesh has no primitives");
    noExtensions(mesh.extensions, "mesh extensions with unhandled references are unsupported");
    const auto targets = mesh.primitives.empty() ? 0 : mesh.primitives[0].targets.size();
    if ((!node.weights.empty() && node.weights.size() != targets) ||
        (!mesh.weights.empty() && mesh.weights.size() != targets)) fail("morph default count is invalid");
    if (validateMesh) for (const auto& primitive : mesh.primitives) {
      if (primitive.targets.size() != targets) fail("mesh morph target counts disagree");
      for (const auto& [name, _] : primitive.extensions)
        if (name != "KHR_materials_variants" || !declared(model, name))
          fail("primitive extension with unhandled references is unsupported");
      index(primitive.material, model.materials.size(), "primitive material is invalid", true);
      const auto check = [&](int accessor, AccessorUsage usage) {
        (void)CheckedAccessor(model, accessor, "subtree clone primitive", {.usage = usage, .allowFloat64 = true});
      };
      for (const auto& [_, accessor] : primitive.attributes) check(accessor, AccessorUsage::VertexAttribute);
      if (primitive.indices != -1) check(primitive.indices, AccessorUsage::Indices);
      for (const auto& target : primitive.targets)
        for (const auto& [_, accessor] : target) check(accessor, AccessorUsage::VertexAttribute);
    }
  }
}

struct NodeChannel {
  int32_t node{-1};
  bool weights{};
  size_t pointerEnd{};
};
NodeChannel nodeChannel(const tinygltf::AnimationChannel& channel) {
  if (channel.target_path != "pointer")
    return {channel.target_node, channel.target_path == "weights", 0};
  const auto found = channel.target_extensions.find("KHR_animation_pointer");
  if (found == channel.target_extensions.end()) fail("animation pointer extension is absent");
  const auto& value = field(found->second, "pointer");
  if (!value.IsString()) fail("animation pointer is not a string");
  const auto& text = value.Get<std::string>();
  if (!text.starts_with("/nodes/")) return {};
  const auto end = text.find('/', 7);
  if (end == std::string::npos || end == 7 || (end > 8 && text[7] == '0'))
    fail("node pointer index is malformed");
  int32_t node{};
  const auto parsed = std::from_chars(text.data() + 7, text.data() + end, node);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data() + end || node < 0)
    fail("node pointer index is invalid");
  const auto suffix = std::string_view(text).substr(end);
  return {node, suffix == "/weights" || suffix.starts_with("/weights/"), end};
}
void remapChannel(tinygltf::AnimationChannel& channel, const NodeChannel& target, int32_t node) {
  if (!target.pointerEnd) { channel.target_node = node; return; }
  auto& extension = channel.target_extensions.at("KHR_animation_pointer");
  const auto& old = extension.Get("pointer").Get<std::string>();
  auto text = "/nodes/" + std::to_string(node) + old.substr(target.pointerEnd);
  extension.Get<Object>()["pointer"] = Value(std::move(text));
  channel.target_extensions_json_string.clear();
}
uint64_t animationCost(const tinygltf::Model& model, const GltfMergeLimits& limits) {
  uint64_t bytes{}, channels{};
  for (const auto& animation : model.animations) {
    noExtensions(animation.extensions, "animation extensions are unsupported");
    for (const auto& sampler : animation.samplers)
      noExtensions(sampler.extensions, "animation sampler extensions are unsupported");
    for (const auto& channel : animation.channels) {
      charge(channels, 1, limits.maxAnimationChannels, "animation channel budget exceeded");
      noExtensions(channel.extensions, "animation channel extensions are unsupported");
      for (const auto& [name, value] : channel.target_extensions) {
        if (name != "KHR_animation_pointer" || !value.IsObject() || !declared(model, name))
          fail("unsupported or undeclared animation target extension");
      }
      index(channel.sampler, animation.samplers.size(), "animation sampler index is invalid");
      const auto& sampler = animation.samplers[size_t(channel.sampler)];
      for (const auto accessor : {sampler.input, sampler.output}) {
        CheckedAccessor view(model, accessor, "subtree clone animation", {.allowFloat64 = true});
        charge(bytes, multiply(multiply(view.count(), view.components()), sizeof(double)),
            limits.maxAnimationDecodedBytes, "animation decoded budget exceeded");
      }
    }
  }
  return bytes;
}
void resident(const tinygltf::Model& model) {
  for (const auto& buffer : model.buffers)
    if (buffer.data.empty() && !buffer.uri.empty()) fail("source buffer is not resident");
  for (const auto& image : model.images) {
    if (!image.image.empty()) continue;
    index(image.bufferView, model.bufferViews.size(), "source image is not resident");
    const auto& view = model.bufferViews[size_t(image.bufferView)];
    index(view.buffer, model.buffers.size(), "image buffer is invalid");
    const auto size = model.buffers[size_t(view.buffer)].data.size();
    if (!view.byteLength || view.byteOffset > size || view.byteLength > size - view.byteOffset)
      fail("image buffer range is not resident");
  }
}
struct InputCounts { uint64_t objects{}, references{}; };
InputCounts counts(const tinygltf::Model& model, const GltfMergeLimits& limits) {
  InputCounts result;
  auto& objects = result.objects;
  auto& references = result.references;
  uint64_t primitives{};
  for (const auto size : {model.buffers.size(), model.bufferViews.size(), model.accessors.size(),
      model.images.size(), model.samplers.size(), model.textures.size(), model.materials.size(),
      model.meshes.size(), model.nodes.size(), model.skins.size(), model.cameras.size(),
      model.lights.size(), model.animations.size(), model.scenes.size()}) {
    if (size > limits.maxTableEntries || size > INT_MAX) fail("source table exceeds its count limit");
    charge(objects, size, limits.maxTotalObjects, "source object count limit exceeded");
  }
  if (model.nodes.size() > limits.maxNodes) fail("source node count limit exceeded");
  for (const auto& mesh : model.meshes) {
    charge(primitives, mesh.primitives.size(), limits.maxPrimitives, "primitive count limit exceeded");
    charge(objects, mesh.primitives.size(), limits.maxTotalObjects, "primitive object count limit exceeded");
  }
  for (const auto& node : model.nodes)
    charge(references, node.children.size(), limits.maxReferences, "node edge budget exceeded");
  for (const auto& skin : model.skins)
    charge(references, add(skin.joints.size(), 2), limits.maxReferences, "skin reference budget exceeded");
  for (const auto& animation : model.animations) {
    charge(references, multiply(animation.channels.size(), 2), limits.maxReferences, "animation reference budget exceeded");
    charge(objects, add(animation.samplers.size(), animation.channels.size()), limits.maxTotalObjects,
        "animation object count limit exceeded");
  }
  return result;
}
void projectedFlag(tinygltf::Node& node,const char* name,const char* key,bool enabled) {
  auto found=node.extensions.find(name);
  if(found==node.extensions.end()) {
    if(enabled) return;
    found=node.extensions.emplace(name,Value(Object{})).first;
  }
  if(!found->second.IsObject()) fail("node flag extension is malformed");
  auto& object=found->second.Get<Object>();
  const auto leaf=object.find(key);
  if(!enabled || (leaf!=object.end() && (!leaf->second.IsBool() || leaf->second.Get<bool>()!=enabled)))
    object[key]=Value(enabled);
}
void projectedAttachments(tinygltf::Node& node,GltfCloneNodeRole role,bool binding) {
  if(role!=GltfCloneNodeRole::Visual) {
    node.camera=node.light=node.emitter=-1;
    std::erase_if(node.extensions,[](const auto& entry) {
      return area(entry.first) || entry.first=="KHR_lights_punctual";
    });
  }
  if(!binding) {
    node.mesh=node.skin=-1;node.weights.clear();node.extensions.erase("EXT_mesh_gpu_instancing");
  }
}
} // namespace

struct GltfProjectedCloneOwnershipProof::State {
  struct Binding {
    int32_t mesh{-1},skin{-1},camera{-1},light{-1},emitter{-1};
    std::optional<Value> instancing;
  };
  GltfSubtreeCloneProjection projection;
  std::vector<Binding> bindings;
  std::vector<tinygltf::Scene> scenes;
  size_t sourceNodes{},sourceSkins{};
  uint64_t bytes{};
};

GltfProjectedCloneOwnershipProof::GltfProjectedCloneOwnershipProof(const tinygltf::Model& source,
    const GltfSubtreeCloneProjection& projection,uint64_t maxRetainedBytes) {
  GltfMergeLimits limits;limits.maxMetadataBytes=maxRetainedBytes;
  MetadataCost cost{limits};cost.charge(sizeof(State));
  cost.charge(multiply(projection.nodes.size(),sizeof(State::Binding)+sizeof(GltfProjectedCloneNode)));
  cost.charge(multiply(projection.affectedScenes.size(),sizeof(size_t)));
  for(const auto& scene:source.scenes) {cost.object(scene);cost.numbers(scene.nodes);cost.numbers(scene.audioEmitters);}
  cost.charge(multiply(source.nodes.size(),sizeof(uint32_t)));
  for(const auto& row:projection.nodes) {
    index(row.sourceNode,source.nodes.size(),"ownership source node is invalid");cost.text(row.name);
    const auto& extensions=source.nodes[size_t(row.sourceNode)].extensions;
    if(const auto found=extensions.find("EXT_mesh_gpu_instancing");found!=extensions.end()) cost.value(found->second);
  }
  auto state=std::make_shared<State>();state->projection=projection;
  state->scenes=source.scenes;
  if(source.scenes.empty() && !projection.nodes.empty()) {
    const auto catalog=scene::BuildGltfSceneCatalog(source,{.maxScenes=1,.maxRootReferences=source.nodes.size()});
    tinygltf::Scene implicit;implicit.nodes.assign(catalog.entries[0].sourceRoots.begin(),catalog.entries[0].sourceRoots.end());
    state->scenes.push_back(std::move(implicit));
  }
  state->sourceNodes=source.nodes.size();state->sourceSkins=source.skins.size();state->bytes=cost.bytes;
  state->bindings.reserve(projection.nodes.size());
  for(const auto& row:projection.nodes) {
    const auto& node=source.nodes[size_t(row.sourceNode)];
    State::Binding binding{node.mesh,node.skin,node.camera,node.light,node.emitter};
    if(const auto found=node.extensions.find("EXT_mesh_gpu_instancing");found!=node.extensions.end())
      binding.instancing=found->second;
    state->bindings.push_back(std::move(binding));
  }
  state_=std::move(state);
}

uint64_t GltfProjectedCloneOwnershipProof::retainedBytes() const noexcept { return state_?state_->bytes:0; }

void GltfProjectedCloneOwnershipProof::validate(const tinygltf::Model& current,
    const GltfProjectedSubtreeClonePlan& projected) const {
  if(!state_) fail("graph ownership proof is absent");
  const auto& proof=*state_;const auto& plan=projected.clone;const auto& projection=proof.projection;
  if(current.nodes.size()!=proof.sourceNodes || current.skins.size()!=proof.sourceSkins ||
      plan.nodes.size()!=proof.sourceNodes || plan.contextOnly.size()!=proof.sourceNodes ||
      projected.roles.size()!=proof.sourceNodes || projected.retainMeshBindings.size()!=proof.sourceNodes ||
      projected.copyWeightTracks.size()!=proof.sourceNodes || plan.skins.size()!=proof.sourceSkins ||
      plan.affectedScenes!=projection.affectedScenes || plan.destinationScene!=projection.destinationScene ||
      plan.clonedNodes!=projection.nodes.size() || plan.root!=-1 || plan.parent!=-1)
    fail("graph ownership identity/counts changed");
  std::vector<int32_t> expected(proof.sourceNodes,-1),skins(proof.sourceSkins,-1),contexts;
  for(size_t p=0;p<projection.nodes.size();++p) {
    const auto& row=projection.nodes[p];const auto source=size_t(row.sourceNode);expected[source]=int32_t(proof.sourceNodes+p);
    const auto& original=current.nodes[source];const auto& binding=proof.bindings[p];
    const auto instancing=original.extensions.find("EXT_mesh_gpu_instancing");
    if(original.mesh!=binding.mesh || original.skin!=binding.skin || original.camera!=binding.camera ||
        original.light!=binding.light || original.emitter!=binding.emitter ||
        bool(instancing!=original.extensions.end())!=binding.instancing.has_value() ||
        (binding.instancing && !exactValue(instancing->second,*binding.instancing))) fail("graph ownership source attachment changed");
    if(projected.roles[source]!=row.role || projected.retainMeshBindings[source]!=row.retainMeshBinding ||
        projected.copyWeightTracks[source]!=row.retainMeshBinding ||
        plan.contextOnly[source]!=(row.role==GltfCloneNodeRole::JointTransformOnly)) fail("graph ownership role/mask changed");
    if(row.retainMeshBinding && original.skin>=0) {
      index(original.skin,skins.size(),"graph ownership skin is invalid");skins[size_t(original.skin)]=0;
    }
    if(row.parentSourceNode<0 && row.role!=GltfCloneNodeRole::Visual) contexts.push_back(expected[source]);
  }
  if(plan.nodes!=expected || plan.contextRoots!=contexts) fail("graph ownership tail mapping changed");
  std::vector<std::vector<int>> expectedChildren(projection.nodes.size());
  for(const auto& child:projection.nodes) if(child.parentSourceNode>=0) {
    index(child.parentSourceNode,expected.size(),"graph ownership projected parent is invalid");
    const auto parent=expected[size_t(child.parentSourceNode)];
    if(parent<0) fail("graph ownership projected parent is unmapped");
    expectedChildren[size_t(parent)-proof.sourceNodes].push_back(expected[size_t(child.sourceNode)]);
  }
  for(size_t n=0;n<expected.size();++n) if(expected[n]<0 && (plan.contextOnly[n] ||
      projected.retainMeshBindings[n] || projected.copyWeightTracks[n])) fail("unmapped ownership mask is set");
  size_t skinCount{};
  for(auto& skin:skins) if(skin==0) skin=int32_t(proof.sourceSkins+skinCount++);
  if(plan.skins!=skins || plan.clonedSkins!=skinCount) fail("graph ownership skin mapping changed");
  std::vector<bool> affectedScenes(proof.scenes.size());std::vector<int32_t> projectedRoots;
  for(const auto scene:projection.affectedScenes) {
    if(scene>=affectedScenes.size()) {
      if(proof.scenes.empty() && projection.nodes.empty() && scene==0) continue;
      fail("graph ownership scene selection is invalid");
    }
    affectedScenes[scene]=true;
  }
  for(const auto& row:projection.nodes) if(row.parentSourceNode<0) projectedRoots.push_back(expected[size_t(row.sourceNode)]);
  for(const auto* output:{&plan.authoredSource,&plan.evaluatedSource}) {
    if(output->nodes.size()!=proof.sourceNodes+projection.nodes.size() || output->skins.size()!=proof.sourceSkins+skinCount)
      fail("graph ownership output tables changed");
    if(output->scenes.size()!=proof.scenes.size()) fail("graph ownership scene table changed");
    for(size_t s=0;s<proof.scenes.size();++s) {
      const auto& original=proof.scenes[s];const auto& scene=output->scenes[s];
      if(scene.name!=original.name || scene.audioEmitters!=original.audioEmitters || !exactMetadata(scene,original))
        fail("graph ownership scene metadata changed");
      const bool affected=affectedScenes[s];const auto roots=affected?projectedRoots.size():0;
      if(scene.nodes.size()!=original.nodes.size()+roots ||
          !std::equal(original.nodes.begin(),original.nodes.end(),scene.nodes.begin())) fail("graph ownership scene root prefix changed");
      size_t next=original.nodes.size();
      if(affected) for(const auto root:projectedRoots)
        if(scene.nodes[next++]!=root) fail("graph ownership scene root tail changed");
    }
    for(size_t p=0;p<projection.nodes.size();++p) {
      const auto& row=projection.nodes[p];const auto& original=current.nodes[size_t(row.sourceNode)];
      const auto& node=output->nodes[proof.sourceNodes+p];const bool visual=row.role==GltfCloneNodeRole::Visual;
      if(node.mesh!=(row.retainMeshBinding?original.mesh:-1) ||
          node.skin!=(row.retainMeshBinding && original.skin>=0?skins[size_t(original.skin)]:-1) ||
          node.camera!=(visual?original.camera:-1) || node.light!=(visual?original.light:-1) ||
          node.emitter!=(visual?original.emitter:-1) || node.name!=row.name ||
          (!row.retainMeshBinding && !node.weights.empty())) fail("graph ownership output attachment changed");
      if(row.retainMeshBinding && !node.weights.empty()) {
        index(original.mesh,current.meshes.size(),"graph ownership mesh is invalid");
        const auto& primitives=current.meshes[size_t(original.mesh)].primitives;
        if(primitives.empty() || node.weights.size()!=primitives[0].targets.size())
          fail("graph ownership output morph shape changed");
      }
      if(node.children!=expectedChildren[p]) fail("graph ownership projected adjacency changed");
      const auto flag=[&](const char* extension,const char* property,bool wanted) {
        const auto found=node.extensions.find(extension);
        if(found==node.extensions.end()) {if(!wanted) fail("graph ownership flag changed");return;}
        if(!found->second.IsObject()) fail("graph ownership flag extension is malformed");
        const auto& object=found->second.Get<Object>();const auto value=object.find(property);
        if(value==object.end()) {if(!wanted) fail("graph ownership default flag changed");return;}
        if(!value->second.IsBool() || value->second.Get<bool>()!=wanted) fail("graph ownership local flag changed");
      };
      flag("KHR_node_visibility","visible",row.visible);flag("KHR_node_selectability","selectable",row.selectable);
      flag("KHR_node_hoverability","hoverable",row.hoverable);
      for(const auto& [name,value]:original.extensions) if(visualExtension(name)) {
        const bool keep=name=="EXT_mesh_gpu_instancing"?row.retainMeshBinding:visual;
        const auto found=node.extensions.find(name);
        if(keep?(found==node.extensions.end() || !exactValue(found->second,value)):found!=node.extensions.end())
          fail("graph ownership output extension attachment changed");
      }
      for(const auto& [name,_]:node.extensions) if(visualExtension(name) && !original.extensions.contains(name))
        fail("graph ownership invented extension attachment");
    }
    for(size_t s=0;s<skins.size();++s) if(skins[s]>=0) {
      auto expectedSkin=current.skins[s];
      for(auto& joint:expectedSkin.joints) {
        index(joint,expected.size(),"graph ownership skin joint is invalid");joint=expected[size_t(joint)];
        if(joint<0) fail("graph ownership skin joint is unmapped");
      }
      if(expectedSkin.skeleton>=0) {
        index(expectedSkin.skeleton,expected.size(),"graph ownership skeleton is invalid");
        expectedSkin.skeleton=expected[size_t(expectedSkin.skeleton)];
        if(expectedSkin.skeleton<0) fail("graph ownership skeleton is unmapped");
      }
      const auto& actual=output->skins[size_t(skins[s])];
      if(actual.name!=expectedSkin.name || actual.inverseBindMatrices!=expectedSkin.inverseBindMatrices ||
          actual.joints!=expectedSkin.joints || actual.skeleton!=expectedSkin.skeleton ||
          !exactMetadata(actual,expectedSkin)) fail("graph ownership appended skin changed");
    }
  }
}

GltfProjectedSubtreeClonePlan PlanGltfProjectedSubtreeClone(const tinygltf::Model& live,
    const tinygltf::Model& archive,const GltfSubtreeCloneProjection& projection,
    const GltfSubtreeCloneRestrictions& restrictions,const GltfMergeLimits& limits) {
  if(!limits.maxTableEntries || !limits.maxMetadataBytes || !limits.maxMetadataDepth)
    fail("zero preparation budget");
  counts(live,limits);const auto inputCounts=counts(archive,limits);
  if(projection.nodes.size()>limits.maxPlacementNodes || projection.nodes.size()>archive.nodes.size() ||
      projection.affectedScenes.size()>limits.maxScenes || restrictions.unsupportedSourceNodes.size()>limits.maxReferences)
    fail("projection table exceeds its bound");
  const auto liveMetadata=metadataCost(live,limits),archiveMetadata=metadataCost(archive,limits);
  const auto workspace=add(multiply(archive.nodes.size(),64),
      add(multiply(archive.skins.size(),8),multiply(projection.nodes.size(),64)));
  if(add(add(liveMetadata,multiply(archiveMetadata,3)),workspace)>limits.maxMetadataBytes)
    fail("projected metadata/workspace budget exceeded before allocation");
  const auto peakResident=add(residentBytes(live),multiply(residentBytes(archive),3));
  if(peakResident>limits.maxResidentBytes) fail("inputs and both projected outputs exceed resident budget");
  validatePrefix(live,archive,limits);resident(archive);
  const auto catalog=scene::BuildGltfSceneCatalog(archive,{.maxScenes=limits.maxScenes,.maxRootReferences=limits.maxRootReferences});
  if(projection.destinationScene>=catalog.entries.size()) fail("projection destination scene is invalid");
  std::vector<bool> affected(catalog.entries.size()),restricted(archive.nodes.size());
  for(const auto scene:projection.affectedScenes) {
    if(scene>=affected.size() || affected[scene]) fail("projection scene is invalid or repeated");
    affected[scene]=true;
  }
  if(!affected[projection.destinationScene]) fail("projection does not include its destination scene");
  for(const auto node:restrictions.unsupportedSourceNodes) {
    index(node,restricted.size(),"restricted source identity is invalid");restricted[size_t(node)]=true;
  }
  GltfProjectedSubtreeClonePlan output;auto& result=output.clone;
  const auto size=archive.nodes.size();
  result.nodes.assign(size,-1);result.contextOnly.assign(size,false);result.skins.assign(archive.skins.size(),-1);
  output.roles.assign(size,GltfCloneNodeRole::JointTransformOnly);
  output.retainMeshBindings.assign(size,false);output.copyWeightTracks.assign(size,false);
  MetadataCost extra{limits};uint64_t references=inputCounts.references;
  std::vector<int32_t> rows(size,-1);
  for(size_t p=0;p<projection.nodes.size();++p) {
    const auto& row=projection.nodes[p];index(row.sourceNode,size,"projected source identity is invalid");
    const auto original=size_t(row.sourceNode);
    if(rows[original]>=0 || restricted[original]) fail("projected source is repeated or caller restricted");
    if(row.role!=GltfCloneNodeRole::Visual && row.role!=GltfCloneNodeRole::MeshBindingOnly &&
        row.role!=GltfCloneNodeRole::JointTransformOnly) fail("projected role is invalid");
    if((row.role==GltfCloneNodeRole::JointTransformOnly && row.retainMeshBinding) ||
        (row.role==GltfCloneNodeRole::MeshBindingOnly && !row.retainMeshBinding))
      fail("projected role and mesh binding disagree");
    if(row.retainMeshBinding && archive.nodes[original].mesh<0) fail("projected mesh binding has no source mesh");
    const auto appended=add(size,p);
    if(appended>=INT_MAX || appended>=limits.maxNodes || appended>=limits.maxTableEntries)
      fail("projected node tail exceeds its count bound");
    rows[original]=int32_t(p);result.nodes[original]=int32_t(appended);
    result.contextOnly[original]=row.role==GltfCloneNodeRole::JointTransformOnly;
    output.roles[original]=row.role;output.retainMeshBindings[original]=row.retainMeshBinding;
    output.copyWeightTracks[original]=row.retainMeshBinding;
    extra.object(archive.nodes[original]);extra.object(live.nodes[original]);extra.text(row.name);
  }
  result.clonedNodes=projection.nodes.size();
  std::vector<std::vector<int32_t>> children(projection.nodes.size());
  std::vector<int32_t> rootRows;
  for(size_t p=0;p<projection.nodes.size();++p) {
    const auto parent=projection.nodes[p].parentSourceNode;
    if(parent==-1) rootRows.push_back(int32_t(p));
    else {
      index(parent,size,"projected parent source is invalid");
      if(rows[size_t(parent)]<0 || parent==projection.nodes[p].sourceNode) fail("projected parent is absent or self");
      children[size_t(rows[size_t(parent)])].push_back(int32_t(p));
      charge(references,1,limits.maxReferences,"projected edges exceed reference budget");
    }
  }
  auto work=rootRows;size_t visited{};
  while(!work.empty()) {
    const auto p=work.back();work.pop_back();++visited;
    work.insert(work.end(),children[size_t(p)].begin(),children[size_t(p)].end());
  }
  if(visited!=projection.nodes.size()) fail("projected source forest contains a cycle");
  std::vector<bool> validatedMeshes(archive.meshes.size());
  for(const auto& row:projection.nodes) {
    const auto original=size_t(row.sourceNode);const auto mesh=archive.nodes[original].mesh;
    const bool validateMesh=mesh>=0 && size_t(mesh)<validatedMeshes.size() && !validatedMeshes[size_t(mesh)];
    validateCloneNode(archive,archive.nodes[original],validateMesh);validateCloneNode(live,live.nodes[original],false);
    if(validateMesh) validatedMeshes[size_t(mesh)]=true;
    if(!row.retainMeshBinding || archive.nodes[original].skin<0) continue;
    const auto skin=archive.nodes[original].skin;index(skin,archive.skins.size(),"projected skin is invalid");
    if(result.skins[size_t(skin)]==0) continue;
    result.skins[size_t(skin)]=0;
    const auto& definition=archive.skins[size_t(skin)];noExtensions(definition.extensions,"skin extensions are unsupported");
    if(definition.joints.empty()) fail("projected skin has no joints");
    std::vector<int> unique=definition.joints;std::ranges::sort(unique);
    if(std::adjacent_find(unique.begin(),unique.end())!=unique.end()) fail("projected skin repeats a joint");
    for(const auto joint:definition.joints) {
      index(joint,size,"projected joint is invalid");
      if(result.nodes[size_t(joint)]<0) fail("projected skin joint is outside the supplied closure");
    }
    index(definition.skeleton,size,"projected skin skeleton is invalid",true);
    if(definition.skeleton>=0 && result.nodes[size_t(definition.skeleton)]<0)
      fail("projected skeleton is outside the supplied closure");
    if(definition.inverseBindMatrices>=0) {
      CheckedAccessor ibm(archive,definition.inverseBindMatrices,"projected inverse binds",{.allowFloat64=true});
      if(ibm.components()!=16 || ibm.count()!=definition.joints.size()) fail("projected inverse bind shape is invalid");
      double matrix[16];for(size_t j=0;j<ibm.count();++j) ibm.readDoubles(j,matrix);
    }
    extra.object(definition);charge(references,add(definition.joints.size(),2),limits.maxReferences,"projected skin refs exceed budget");
  }
  for(size_t s=0;s<result.skins.size();++s) if(result.skins[s]==0) {
    const auto appended=add(archive.skins.size(),result.clonedSkins);
    if(appended>=INT_MAX || appended>=limits.maxTableEntries) fail("projected skin tail exceeds its count bound");
    result.skins[s]=int32_t(appended);++result.clonedSkins;
  }
  const auto decoded=animationCost(archive,limits);uint64_t clonedDecoded{},originalChannels{};
  for(const auto& animation:archive.animations) {
    result.originalChannelCounts.push_back(animation.channels.size());originalChannels=add(originalChannels,animation.channels.size());
    for(const auto& channel:animation.channels) {
      const auto target=nodeChannel(channel);
      if(target.node<0) continue;
      index(target.node,size,"projected animation node is invalid");
      if(result.nodes[size_t(target.node)]<0 || (target.weights && !output.copyWeightTracks[size_t(target.node)])) continue;
      charge(result.clonedChannels,1,limits.maxClonedChannels,"projected channel count exceeds budget");
      charge(references,2,limits.maxReferences,"projected channel refs exceed budget");
      extra.object(channel);extra.extensions(channel.target_extensions);extra.value(channel.target_extras);
      const auto& sampler=animation.samplers[size_t(channel.sampler)];
      for(const auto accessor:{sampler.input,sampler.output}) {
        CheckedAccessor view(archive,accessor,"projected channel",{.allowFloat64=true});
        charge(clonedDecoded,multiply(multiply(view.count(),view.components()),sizeof(double)),
            limits.maxAnimationDecodedBytes,"projected animation decode budget exceeded");
      }
    }
  }
  result.animationDecodedBytes=add(decoded,clonedDecoded);
  if(result.animationDecodedBytes>limits.maxAnimationDecodedBytes ||
      add(originalChannels,result.clonedChannels)>limits.maxAnimationChannels) fail("combined projected animation exceeds budget");
  uint64_t rootReferences{};
  for(const auto& scene:catalog.entries) rootReferences=add(rootReferences,scene.sourceRoots.size());
  rootReferences=add(rootReferences,multiply(rootRows.size(),projection.affectedScenes.size()));
  if(rootReferences>limits.maxRootReferences) fail("projected scene roots exceed budget");
  charge(references,multiply(rootRows.size(),projection.affectedScenes.size()),limits.maxReferences,"projected root refs exceed budget");
  const auto appendedObjects=add(add(result.clonedNodes,result.clonedSkins),result.clonedChannels);
  if(add(add(inputCounts.objects,appendedObjects),archive.scenes.empty() && !projection.nodes.empty()?1:0)>limits.maxTotalObjects)
    fail("projected object count exceeds budget");
  result.metadataBytes=add(add(liveMetadata,multiply(archiveMetadata,3)),
      add(multiply(extra.bytes,2),add(workspace,multiply(rootReferences,16))));
  if(result.metadataBytes>limits.maxMetadataBytes) fail("projected input/output metadata exceeds budget");
  {const auto animations=ParseAnimations(archive);if(!animations.diagnostics.empty()) fail("unsupported animation cannot be projected faithfully");}
  (void)ParseGltfNodeFlags(archive,{.maxNodes=limits.maxNodes,.maxRetainedBytes=limits.maxMetadataBytes});
  (void)ParseMeshGpuInstancing(archive,{.maxInstances=limits.maxReferences,.maxExpandedPrimitives=limits.maxTotalObjects,
      .maxDecodedBytes=limits.maxAccessorDecodedBytes});
  result.destinationScene=projection.destinationScene;result.affectedScenes=projection.affectedScenes;
  result.root=result.parent=-1;result.peakResidentBytes=peakResident;
  for(const auto p:rootRows) if(projection.nodes[size_t(p)].role!=GltfCloneNodeRole::Visual)
    result.contextRoots.push_back(result.nodes[size_t(projection.nodes[size_t(p)].sourceNode)]);
  const auto assemble=[&](bool evaluated) {
    auto model=archive;
    if(evaluated) {
      std::copy(live.nodes.begin(),live.nodes.end(),model.nodes.begin());
      std::copy(live.materials.begin(),live.materials.end(),model.materials.begin());
      std::copy(live.lights.begin(),live.lights.end(),model.lights.begin());
      std::copy(live.cameras.begin(),live.cameras.end(),model.cameras.begin());model.defaultScene=live.defaultScene;
    }
    const auto& source=evaluated?live:archive;
    model.nodes.reserve(size+projection.nodes.size());model.skins.reserve(archive.skins.size()+size_t(result.clonedSkins));
    for(size_t p=0;p<projection.nodes.size();++p) {
      const auto& row=projection.nodes[p];auto node=source.nodes[size_t(row.sourceNode)];node.children.clear();
      for(const auto child:children[p]) node.children.push_back(result.nodes[size_t(projection.nodes[size_t(child)].sourceNode)]);
      projectedAttachments(node,row.role,row.retainMeshBinding);
      if(node.skin>=0) node.skin=result.skins[size_t(node.skin)];
      node.name=row.name;projectedFlag(node,"KHR_node_visibility","visible",row.visible);
      projectedFlag(node,"KHR_node_selectability","selectable",row.selectable);
      projectedFlag(node,"KHR_node_hoverability","hoverable",row.hoverable);node.extensions_json_string.clear();
      model.nodes.push_back(std::move(node));
    }
    for(size_t s=0;s<result.skins.size();++s) if(result.skins[s]>=0) {
      auto skin=archive.skins[s];for(auto& joint:skin.joints) joint=result.nodes[size_t(joint)];
      if(skin.skeleton>=0) skin.skeleton=result.nodes[size_t(skin.skeleton)];model.skins.push_back(std::move(skin));
    }
    for(auto& animation:model.animations) {
      const auto count=animation.channels.size();
      for(size_t c=0;c<count;++c) {
        const auto target=nodeChannel(animation.channels[c]);
        if(target.node<0 || result.nodes[size_t(target.node)]<0 ||
            (target.weights && !output.copyWeightTracks[size_t(target.node)])) continue;
        auto channel=animation.channels[c];remapChannel(channel,target,result.nodes[size_t(target.node)]);
        animation.channels.push_back(std::move(channel));
      }
    }
    if(!projection.nodes.empty() && model.scenes.empty()) {
      tinygltf::Scene scene;scene.nodes.assign(catalog.entries[0].sourceRoots.begin(),catalog.entries[0].sourceRoots.end());
      model.scenes.push_back(std::move(scene));
    }
    for(const auto scene:projection.affectedScenes) for(const auto p:rootRows)
      model.scenes[scene].nodes.push_back(result.nodes[size_t(projection.nodes[size_t(p)].sourceNode)]);
    for(const auto* extension:{"KHR_node_visibility","KHR_node_selectability","KHR_node_hoverability"})
      if(std::ranges::any_of(model.nodes,[&](const auto& node) {return node.extensions.contains(extension);}) &&
          std::ranges::find(model.extensionsUsed,extension)==model.extensionsUsed.end()) model.extensionsUsed.emplace_back(extension);
    return model;
  };
  result.authoredSource=assemble(false);result.evaluatedSource=assemble(true);
  return output;
}
} // namespace container::geometry::gltf
