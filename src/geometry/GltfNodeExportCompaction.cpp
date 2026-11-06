#include <Container/geometry/GltfNodeExportCompaction.h>

#include <Container/geometry/GltfAccessor.h>
#include <Container/geometry/GltfAnimation.h>
#include <Container/geometry/GltfMaterialVariants.h>
#include <Container/geometry/GltfSceneCatalog.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <climits>
#include <cmath>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>

namespace container::geometry::gltf {
namespace {
using Value = tinygltf::Value;
using Object = Value::Object;
using Names = std::initializer_list<std::string_view>;
constexpr std::array kAreas{"KHR_lights_area", "EXT_lights_area", "WEBGI_lights_area", "WEBGI_area_light"};
constexpr std::array kMaterials{"KHR_materials_unlit", "KHR_materials_specular", "KHR_materials_ior",
    "KHR_materials_dispersion", "KHR_materials_emissive_strength", "KHR_materials_transmission",
    "KHR_materials_volume", "KHR_materials_clearcoat", "KHR_materials_sheen", "KHR_materials_iridescence",
    "KHR_materials_anisotropy", "KHR_materials_diffuse_transmission", "KHR_materials_pbrSpecularGlossiness"};

[[noreturn]] void fail(std::string_view message) {
  throw std::runtime_error("glTF node export compaction: " + std::string(message));
}
uint64_t add(uint64_t a, uint64_t b) {
  if (b > UINT64_MAX-a) fail("budget integer overflow");
  return a+b;
}
uint64_t mul(uint64_t a, uint64_t b) {
  if (a && b > UINT64_MAX/a) fail("budget integer overflow");
  return a*b;
}
void charge(uint64_t& total, uint64_t bytes, uint64_t limit, const char* message) {
  total = add(total,bytes);
  if (total > limit) fail(message);
}
void index(int id, size_t count, const char* message, bool optional = false) {
  if ((optional && id == -1) || (id >= 0 && size_t(id) < count)) return;
  fail(message);
}
bool named(std::string_view name, Names names) { return std::ranges::find(names,name) != names.end(); }
bool area(std::string_view name) { return std::ranges::find(kAreas,name) != kAreas.end(); }
bool knownDeclaration(std::string_view name) {
  return area(name) || std::ranges::find(kMaterials,name) != kMaterials.end() || named(name,{
      "KHR_texture_transform", "KHR_node_visibility", "KHR_node_selectability", "KHR_node_hoverability",
      "KHR_lights_punctual", "KHR_texture_basisu", "EXT_texture_webp", "MSFT_texture_dds",
      "KHR_mesh_quantization", "KHR_accessor_float64", "EXT_mesh_gpu_instancing", "KHR_animation_pointer",
      "KHR_materials_variants"});
}

// Bounds logical metadata before any model-sized copy or semantic parser. Each
// opaque Value is visited solely for size/depth/finite validation, not remapped.
struct Budget {
  const GltfMergeLimits& limits;
  const tinygltf::Model& source;
  uint64_t metadata{}, objects{}, references{}, payload{}, primitives{}, channels{}, decoded{}, animationDecoded{};
  void bytes(uint64_t n) { charge(metadata,n,limits.maxMetadataBytes,"metadata budget exceeded"); }
  void text(std::string_view s) { bytes(add(s.size(),32)); }
  void refs(uint64_t n) { charge(references,n,limits.maxReferences,"reference budget exceeded"); }
  void finite(double v) { if (!std::isfinite(v)) fail("nonfinite metadata"); }
  template<class T> void numbers(const std::vector<T>& values) {
    bytes(add(mul(values.size(),sizeof(T)),32));
    for (const auto v : values) if constexpr (std::is_floating_point_v<T>) finite(v);
  }
  void value(const Value& v, uint32_t depth = 0) {
    if (depth > limits.maxMetadataDepth) fail("metadata depth exceeded");
    bytes(sizeof(Value)+32);
    if (v.IsString()) text(v.Get<std::string>());
    else if (v.IsNumber()) finite(v.GetNumberAsDouble());
    else if (v.IsBinary()) bytes(v.Get<std::vector<unsigned char>>().size());
    else if (v.IsArray()) for (const auto& child : v.Get<Value::Array>()) value(child,depth+1);
    else if (v.IsObject()) for (const auto& [key,child] : v.Get<Object>()) {
      bytes(64); text(key); value(child,depth+1);
    }
  }
  // A known extension can contain schema-defined texture/light extensions.
  // Unknown nested extension maps reject; extras remain uninterpreted.
  void nested(const Value& v, bool transforms, bool areas) {
    if (v.IsObject()) for (const auto& [key,child] : v.Get<Object>()) {
      if (key == "extras") continue;
      if (key == "extensions") {
        if (!child.IsObject()) fail("nested extensions must be an object");
        for (const auto& [name,extension] : child.Get<Object>()) {
          if (!(transforms && name == "KHR_texture_transform") && !(areas && area(name)))
            fail("unsupported nested extension reference schema");
          if (!extension.IsObject()) fail("nested extension must be an object");
          if (std::ranges::find(source.extensionsUsed,name) == source.extensionsUsed.end())
            fail("nested extension lacks extensionsUsed declaration");
          nested(extension,false,false);
        }
      } else nested(child,transforms,areas);
    } else if (v.IsArray()) for (const auto& child : v.Get<Value::Array>()) nested(child,transforms,areas);
  }
  void extensions(const tinygltf::ExtensionMap& extensions, Names allowed = {}, bool transforms = false,
                  bool areas = false, bool materials = false) {
    for (const auto& [name,v] : extensions) {
      if (!named(name,allowed) && !(materials && std::ranges::find(kMaterials,name) != kMaterials.end()))
        fail("unsupported extension reference schema: " + name);
      if (std::ranges::find(source.extensionsUsed,name) == source.extensionsUsed.end())
        fail("extension lacks extensionsUsed declaration");
      if (!v.IsObject()) fail("extension must be an object");
      text(name); value(v); nested(v,transforms,areas);
    }
  }
  template<class T> void property(const T& v, Names allowed = {}, bool transforms = false,
                                bool areas = false, bool materials = false) {
    charge(objects,1,limits.maxTotalObjects,"object budget exceeded");
    bytes(sizeof(T));
    if constexpr (requires { v.name; }) text(v.name);
    if constexpr (requires { v.extras; }) value(v.extras);
    if constexpr (requires { v.extensions; }) extensions(v.extensions,allowed,transforms,areas,materials);
    if constexpr (requires { v.extras_json_string; }) text(v.extras_json_string);
    if constexpr (requires { v.extensions_json_string; }) text(v.extensions_json_string);
  }
  template<class T> void table(const std::vector<T>& values) {
    if (values.size() > limits.maxTableEntries || values.size() > INT_MAX)
      fail("table count exceeds its bound");
  }
};

struct Target { int32_t node{-1}; size_t end{}; };
Target target(const tinygltf::Model& source, const tinygltf::AnimationChannel& channel) {
  if (channel.target_path != "pointer") {
    if (!named(channel.target_path,{"translation","rotation","scale","weights"}))
      fail("unsupported core animation target");
    if (!channel.target_extensions.empty()) fail("core animation target carries an unsupported extension");
    index(channel.target_node,source.nodes.size(),"animation node index is invalid");
    return {channel.target_node,0};
  }
  const auto ext = channel.target_extensions.find("KHR_animation_pointer");
  if (ext == channel.target_extensions.end() || !ext->second.IsObject() ||
      !ext->second.Has("pointer") || !ext->second.Get("pointer").IsString())
    fail("animation pointer is absent or malformed");
  if (channel.target_node != -1) fail("pointer channel also declares a core node target");
  const auto& text = ext->second.Get("pointer").Get<std::string>();
  if (!text.starts_with("/nodes/")) return {};
  const auto end = text.find('/',7);
  if (end == std::string::npos || end == 7 || (end > 8 && text[7] == '0')) fail("node pointer index is malformed");
  int32_t node{};
  const auto parsed = std::from_chars(text.data()+7,text.data()+end,node);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data()+end) fail("node pointer index is invalid");
  index(node,source.nodes.size(),"node pointer is outside the node table");
  return {node,end};
}
void retarget(tinygltf::AnimationChannel& channel, const Target& reference, int32_t node) {
  if (!reference.end) { channel.target_node = node; return; }
  auto& extension = channel.target_extensions.at("KHR_animation_pointer");
  const auto& old = extension.Get("pointer").Get<std::string>();
  auto replacement = "/nodes/" + std::to_string(node) + old.substr(reference.end);
  extension.Get<Object>().at("pointer") = Value(std::move(replacement));
  channel.target_extensions_json_string.clear(); // Only this typed reference changed.
}

void texture(Budget& b, const tinygltf::TextureInfo& info) {
  b.property(info,{"KHR_texture_transform"});
  index(info.index,b.source.textures.size(),"material texture index is invalid",true); b.refs(1);
}
template<class T> void textureOther(Budget& b, const T& info) {
  b.property(info,{"KHR_texture_transform"});
  index(info.index,b.source.textures.size(),"material texture index is invalid",true); b.refs(1);
}

void audit(const tinygltf::Model& source, Budget& b) {
  const auto& limits = b.limits;
  b.bytes(sizeof(source));
  for (const auto size : {source.nodes.size(),source.skins.size(),source.meshes.size(),source.materials.size(),
      source.images.size(),source.textures.size(),source.samplers.size(),source.accessors.size(),source.bufferViews.size(),
      source.buffers.size(),source.animations.size(),source.cameras.size(),source.lights.size(),source.scenes.size()})
    if (size > limits.maxTableEntries || size > INT_MAX) fail("table count exceeds its bound");
  if (source.nodes.size() > limits.maxNodes || source.scenes.size() > limits.maxScenes)
    fail("node/scene count exceeds its bound");
  if (!source.audioEmitters.empty() || !source.audioSources.empty()) fail("audio reference schema is unsupported");
  // Bound all edge counts before the forest validator allocates its workspace.
  for (const auto& node : source.nodes) b.refs(node.children.size());
  uint64_t roots{};
  for (const auto& scene : source.scenes) {
    charge(roots,scene.nodes.size(),limits.maxRootReferences,"scene root budget exceeded"); b.refs(scene.nodes.size());
  }
  if (source.scenes.empty()) {
    // The implicit catalog can have at most one root per source node.
    // The exact root bound is enforced by BuildGltfSceneCatalog below.
    b.bytes(mul(source.nodes.size(),sizeof(int32_t)));
  }
  for (const auto& name : source.extensionsUsed) {
    b.text(name);
    if (!knownDeclaration(name)) fail("unsupported extension declaration: " + name);
  }
  for (size_t i=0;i<source.extensionsUsed.size();++i)
    if (std::find(source.extensionsUsed.begin(),source.extensionsUsed.begin()+i,source.extensionsUsed[i]) !=
        source.extensionsUsed.begin()+i) fail("duplicate extensionsUsed declaration");
  for (const auto& name : source.extensionsRequired) {
    b.text(name);
    if (std::ranges::find(source.extensionsUsed,name) == source.extensionsUsed.end()) fail("required extension is undeclared");
  }
  b.property(source.asset); b.text(source.asset.version); b.text(source.asset.minVersion);
  b.text(source.asset.generator); b.text(source.asset.copyright);
  b.value(source.extras); b.text(source.extras_json_string); b.text(source.extensions_json_string);
  b.extensions(source.extensions,{"KHR_materials_variants","KHR_lights_punctual",
      "KHR_lights_area","EXT_lights_area","WEBGI_lights_area","WEBGI_area_light"},false,true);
  for (const auto& buffer : source.buffers) {
    b.property(buffer); b.text(buffer.uri);
    if (buffer.data.empty() && !buffer.uri.empty()) fail("buffer is not resident");
    charge(b.payload,buffer.data.size(),limits.maxResidentBytes,"resident payload budget exceeded");
  }
  for (const auto& view : source.bufferViews) {
    b.property(view); index(view.buffer,source.buffers.size(),"buffer view buffer is invalid"); b.refs(1);
    const auto size = source.buffers[size_t(view.buffer)].data.size();
    if (view.byteOffset > size || view.byteLength > size-view.byteOffset) fail("buffer view range exceeds its buffer");
  }
  for (size_t a=0;a<source.accessors.size();++a) {
    const auto& accessor = source.accessors[a]; b.property(accessor);
    b.numbers(accessor.minValues); b.numbers(accessor.maxValues);
    if (accessor.sparse.isSparse) {
      b.property(accessor.sparse); b.property(accessor.sparse.indices); b.property(accessor.sparse.values); b.refs(2);
    }
    const CheckedAccessor view(source,int(a),"node export compaction",{.allowFloat64=true});
    b.refs(accessor.bufferView >= 0);
    charge(b.decoded,mul(mul(view.count(),view.components()),sizeof(double)),limits.maxAccessorDecodedBytes,
        "accessor expansion budget exceeded");
  }
  for (const auto& image : source.images) {
    b.property(image); b.text(image.uri); b.text(image.mimeType);
    index(image.bufferView,source.bufferViews.size(),"image buffer view is invalid",true); b.refs(image.bufferView >= 0);
    if (image.image.empty() && image.bufferView < 0) fail("image is not resident");
    charge(b.payload,image.image.size(),limits.maxResidentBytes,"resident payload budget exceeded");
  }
  for (const auto& sampler : source.samplers) b.property(sampler);
  for (const auto& tex : source.textures) {
    b.property(tex,{"KHR_texture_basisu","EXT_texture_webp","MSFT_texture_dds"});
    index(tex.source,source.images.size(),"texture source is invalid",true);
    index(tex.sampler,source.samplers.size(),"texture sampler is invalid",true); b.refs(2);
  }
  for (const auto& material : source.materials) {
    b.property(material,{},true,false,true); b.numbers(material.lods);
    if (!material.lods.empty()) fail("material LOD reference schema is unsupported");
    b.property(material.pbrMetallicRoughness); b.numbers(material.pbrMetallicRoughness.baseColorFactor);
    b.numbers(material.emissiveFactor);
    for (const auto v : {material.pbrMetallicRoughness.metallicFactor,material.pbrMetallicRoughness.roughnessFactor,
        material.alphaCutoff,material.normalTexture.scale,material.occlusionTexture.strength}) b.finite(v);
    texture(b,material.pbrMetallicRoughness.baseColorTexture);
    texture(b,material.pbrMetallicRoughness.metallicRoughnessTexture);
    textureOther(b,material.normalTexture); textureOther(b,material.occlusionTexture); texture(b,material.emissiveTexture);
    for (const auto* map : {&material.values,&material.additionalValues}) for (const auto& [key,p] : *map) {
      b.bytes(sizeof(p)+64); b.text(key); b.text(p.string_value); b.numbers(p.number_array); b.finite(p.number_value);
      for (const auto& [name,v] : p.json_double_value) { b.text(name); b.finite(v); b.bytes(64); }
    }
  }
  for (const auto& mesh : source.meshes) {
    b.property(mesh); b.numbers(mesh.weights);
    charge(b.primitives,mesh.primitives.size(),limits.maxPrimitives,"primitive count exceeds its bound");
    for (const auto& primitive : mesh.primitives) {
      b.property(primitive,{"KHR_materials_variants"});
      index(primitive.material,source.materials.size(),"primitive material is invalid",true);
      index(primitive.indices,source.accessors.size(),"primitive index accessor is invalid",true); b.refs(2);
      for (const auto& [semantic,id] : primitive.attributes) {
        b.text(semantic); b.bytes(64); index(id,source.accessors.size(),"attribute accessor is invalid"); b.refs(1);
      }
      for (const auto& attrs : primitive.targets) {
        b.bytes(sizeof(attrs));
        for (const auto& [semantic,id] : attrs) {
          b.text(semantic); b.bytes(64); index(id,source.accessors.size(),"morph accessor is invalid"); b.refs(1);
        }
      }
    }
  }
  for (const auto& node : source.nodes) {
    b.property(node,{"KHR_lights_punctual","EXT_mesh_gpu_instancing","KHR_node_visibility","KHR_node_selectability",
        "KHR_node_hoverability","KHR_lights_area","EXT_lights_area","WEBGI_lights_area","WEBGI_area_light"});
    if (node.emitter != -1 || !node.lods.empty()) fail("node audio/LOD reference schema is unsupported");
    index(node.mesh,source.meshes.size(),"node mesh is invalid",true);
    index(node.skin,source.skins.size(),"node skin is invalid",true);
    index(node.camera,source.cameras.size(),"node camera is invalid",true);
    index(node.light,source.lights.size(),"node light is invalid",true); b.refs(4);
    b.numbers(node.children); b.numbers(node.matrix); b.numbers(node.translation);
    b.numbers(node.rotation); b.numbers(node.scale); b.numbers(node.weights);
  }
  for (const auto& skin : source.skins) {
    b.property(skin); b.numbers(skin.joints); b.refs(add(skin.joints.size(),2));
    index(skin.skeleton,source.nodes.size(),"skin skeleton is invalid",true);
    index(skin.inverseBindMatrices,source.accessors.size(),"inverse bind accessor is invalid",true);
    if (skin.joints.empty()) fail("skin joint table is empty");
    for (const auto joint : skin.joints) index(joint,source.nodes.size(),"skin joint is invalid");
  }
  for (const auto& camera : source.cameras) {
    b.property(camera); b.text(camera.type); b.property(camera.perspective); b.property(camera.orthographic);
    for (const auto v : {camera.perspective.aspectRatio,camera.perspective.yfov,camera.perspective.znear,camera.perspective.zfar,
        camera.orthographic.xmag,camera.orthographic.ymag,camera.orthographic.znear,camera.orthographic.zfar}) b.finite(v);
  }
  for (const auto& light : source.lights) {
    b.property(light,{"KHR_lights_area","EXT_lights_area","WEBGI_lights_area","WEBGI_area_light"});
    b.property(light.spot); b.text(light.type); b.numbers(light.color);
    for (const auto v : {light.intensity,light.range,light.spot.innerConeAngle,light.spot.outerConeAngle}) b.finite(v);
  }
  for (const auto& animation : source.animations) {
    b.property(animation); b.table(animation.samplers); b.table(animation.channels);
    charge(b.channels,animation.channels.size(),limits.maxAnimationChannels,"animation channel budget exceeded");
    for (const auto& sampler : animation.samplers) {
      b.property(sampler); b.text(sampler.interpolation); b.refs(2);
      index(sampler.input,source.accessors.size(),"animation input accessor is invalid");
      index(sampler.output,source.accessors.size(),"animation output accessor is invalid");
    }
    for (const auto& channel : animation.channels) {
      b.property(channel); b.text(channel.target_path); b.value(channel.target_extras);
      b.text(channel.target_extras_json_string); b.text(channel.target_extensions_json_string);
      b.extensions(channel.target_extensions,{"KHR_animation_pointer"});
      index(channel.sampler,animation.samplers.size(),"animation sampler is invalid"); b.refs(2);
      (void)target(source,channel);
      const auto& sampler = animation.samplers[size_t(channel.sampler)];
      for (const auto id : {sampler.input,sampler.output}) {
        const CheckedAccessor view(source,id,"node export animation",{.allowFloat64=true});
        charge(b.animationDecoded,mul(mul(view.count(),view.components()),sizeof(double)),
            limits.maxAnimationDecodedBytes,"animation expansion budget exceeded");
      }
    }
  }
  for (const auto& scene : source.scenes) {
    b.property(scene); b.numbers(scene.nodes);
    if (!scene.audioEmitters.empty()) fail("scene audio reference schema is unsupported");
  }
}
} // namespace

GltfNodeExportCompactionPlan CompactGltfExportNodes(const tinygltf::Model& source,
    std::span<const int32_t> retiredNodes, const GltfMergeLimits& limits) {
  if (!limits.maxMetadataBytes || !limits.maxMetadataDepth || !limits.maxTableEntries)
    fail("zero preparation budget");
  if (retiredNodes.size() > limits.maxNodes || retiredNodes.size() > source.nodes.size())
    fail("retired node table exceeds its bound");
  Budget budget{limits,source}; audit(source,budget);
  GltfNodeExportCompactionPlan result;
  result.peakPayloadBytes = mul(budget.payload,2);
  if (result.peakPayloadBytes > limits.maxResidentBytes) fail("input and output payloads exceed resident budget");
  // Includes maps, forest traversal, surviving-owner/joint marks, and the
  // temporary vector storage while moved survivors replace the copied table.
  const auto workspace = add(mul(source.nodes.size(),sizeof(tinygltf::Node)+64),
      add(mul(source.skins.size(),sizeof(tinygltf::Skin)+32),mul(budget.references,16)));
  result.metadataBytes = add(mul(budget.metadata,2),workspace);
  if (result.metadataBytes > limits.maxMetadataBytes) fail("input/output metadata and workspace exceed budget");
  result.animationDecodedBytes = budget.animationDecoded;
  const auto catalog = container::scene::BuildGltfSceneCatalog(source,
      {.maxScenes=limits.maxScenes,.maxRootReferences=limits.maxRootReferences});
  (void)catalog;
  std::vector<uint8_t> retired(source.nodes.size());
  for (const auto node : retiredNodes) {
    index(node,source.nodes.size(),"retired node index is invalid");
    if (retired[size_t(node)]) fail("retired node index is repeated");
    retired[size_t(node)] = 1;
  }
  for (size_t n=0;n<source.nodes.size();++n) if (retired[n])
    for (const auto child : source.nodes[n].children)
      if (!retired[size_t(child)]) fail("retirement is not closed over descendants");
  result.oldToNewNodes.assign(source.nodes.size(),-1);
  int32_t next{};
  result.newToOldNodes.reserve(source.nodes.size()-retiredNodes.size());
  for (size_t n=0;n<source.nodes.size();++n) if (!retired[n]) {
    result.oldToNewNodes[n] = next++;
    result.newToOldNodes.push_back(int32_t(n));
  }
  result.retiredNodes = retiredNodes.size();
  std::vector<uint8_t> owned(source.skins.size());
  for (size_t n=0;n<source.nodes.size();++n)
    if (!retired[n] && source.nodes[n].skin >= 0) owned[size_t(source.nodes[n].skin)] = 1;
  result.oldToNewSkins.assign(source.skins.size(),-1);
  std::vector<int32_t> jointStamp(source.nodes.size(),-1);
  next = 0;
  for (size_t s=0;s<source.skins.size();++s) {
    const auto& skin = source.skins[s];
    bool affected = skin.skeleton >= 0 && retired[size_t(skin.skeleton)];
    for (const auto joint : skin.joints) {
      if (jointStamp[size_t(joint)] == int32_t(s)) fail("skin repeats a joint");
      jointStamp[size_t(joint)] = int32_t(s);
      affected = affected || retired[size_t(joint)];
    }
    if (affected) {
      if (owned[s]) fail("surviving skin owner requires a retired joint or skeleton");
      ++result.droppedSkins;
    } else result.oldToNewSkins[s] = next++;
  }
  // Semantic parsers run only after every metadata/count/expansion bound. They
  // are released before output copying. Unknown channels never disappear just
  // because their target happens to be in the retired set.
  (void)ParseMaterialVariants(source);
  {
    const auto library = ParseAnimations(source);
    if (!library.diagnostics.empty()) fail("unsupported animation pointer cannot be compacted safely");
  }
  result.model = source;
  auto& output = result.model;
  std::vector<tinygltf::Node> nodes; nodes.reserve(source.nodes.size()-retiredNodes.size());
  for (size_t n=0;n<source.nodes.size();++n) if (!retired[n]) {
    auto node = std::move(output.nodes[n]);
    std::erase_if(node.children,[&](int child) { return retired[size_t(child)] != 0; });
    for (auto& child : node.children) child = result.oldToNewNodes[size_t(child)];
    if (node.skin >= 0) node.skin = result.oldToNewSkins[size_t(node.skin)];
    nodes.push_back(std::move(node));
  }
  output.nodes = std::move(nodes);
  std::vector<tinygltf::Skin> skins; skins.reserve(source.skins.size()-size_t(result.droppedSkins));
  for (size_t s=0;s<source.skins.size();++s) if (result.oldToNewSkins[s] >= 0) {
    auto skin = std::move(output.skins[s]);
    for (auto& joint : skin.joints) joint = result.oldToNewNodes[size_t(joint)];
    if (skin.skeleton >= 0) skin.skeleton = result.oldToNewNodes[size_t(skin.skeleton)];
    skins.push_back(std::move(skin));
  }
  output.skins = std::move(skins);
  for (auto& scene : output.scenes) {
    std::erase_if(scene.nodes,[&](int root) { return retired[size_t(root)] != 0; });
    for (auto& root : scene.nodes) root = result.oldToNewNodes[size_t(root)];
  }
  for (auto& animation : output.animations) {
    size_t kept{};
    for (size_t c=0;c<animation.channels.size();++c) {
      auto& channel = animation.channels[c]; const auto reference = target(source,channel);
      if (reference.node >= 0 && retired[size_t(reference.node)]) { ++result.droppedChannels; continue; }
      if (reference.node >= 0 && result.oldToNewNodes[size_t(reference.node)] != reference.node)
        retarget(channel,reference,result.oldToNewNodes[size_t(reference.node)]);
      if (kept != c) animation.channels[kept] = std::move(channel);
      ++kept;
    }
    animation.channels.resize(kept);
  }
  return result;
}
} // namespace container::geometry::gltf
