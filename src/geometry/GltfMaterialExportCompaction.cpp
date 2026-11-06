#include <Container/geometry/GltfMaterialExportCompaction.h>

#include <Container/geometry/GltfAccessor.h>
#include <Container/geometry/GltfXmpMetadata.h>

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
#include <utility>

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
  throw std::runtime_error("glTF material export compaction: " + std::string(message));
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
      "KHR_lights_punctual", "KHR_texture_basisu", "EXT_texture_webp", "EXT_texture_avif", "MSFT_texture_dds",
      "KHR_mesh_quantization", "KHR_accessor_float64", "EXT_mesh_gpu_instancing", "KHR_animation_pointer",
      "KHR_materials_variants", "MSFT_lod", "KHR_draco_mesh_compression",
      "EXT_meshopt_compression", "KHR_meshopt_compression", kGltfXmpExtension});
}

// Bounds logical metadata before reference-proof allocation. Each
// opaque Value is visited solely for size/depth/finite validation, not remapped.
struct Budget {
  const GltfMaterialExportCompactionLimits& limits;
  const tinygltf::Model& source;
  uint64_t metadata{}, objects{}, references{}, payload{}, primitives{}, channels{}, decoded{}, animationDecoded{},values{},steps{};
  void work(uint64_t n=1) { charge(steps,n,limits.maxTraversalSteps,"traversal work budget exceeded"); }
  void bytes(uint64_t n) { work(); charge(metadata,n,limits.maxMetadataBytes,"metadata budget exceeded"); }
  void text(std::string_view s) { bytes(add(s.size(),32)); }
  void refs(uint64_t n) { work(n); charge(references,n,limits.maxReferences,"reference budget exceeded"); }
  void finite(double v) { if (!std::isfinite(v)) fail("nonfinite metadata"); }
  template<class T> void numbers(const std::vector<T>& values) {
    work(values.size()); bytes(add(mul(values.size(),sizeof(T)),32));
    for (const auto v : values) if constexpr (std::is_floating_point_v<T>) finite(v);
  }
  void value(const Value& v, uint32_t depth = 0) {
    work(); charge(values,1,limits.maxMetadataValues,"metadata value budget exceeded");
    if ((!v.IsString() && !v.Get<std::string>().empty()) ||
        (!v.IsBinary() && !v.Get<std::vector<unsigned char>>().empty()) ||
        (!v.IsArray() && !v.Get<Value::Array>().empty()) ||
        (!v.IsObject() && !v.Get<Object>().empty())) fail("inactive metadata storage is malformed");
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
    work();
    if (v.IsObject()) for (const auto& [key,child] : v.Get<Object>()) {
      if (key == "extras") continue;
      if (key == "extensions") {
        if (!child.IsObject()) fail("nested extensions must be an object");
        for (const auto& [name,extension] : child.Get<Object>()) {
          if (!(transforms && name == "KHR_texture_transform") && !(areas && area(name)))
            fail("unsupported nested extension reference schema");
          if (!extension.IsObject()) fail("nested extension must be an object");
          work(source.extensionsUsed.size());
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
        fail("unsupported extension reference schema: " + name.substr(0,256));
      work(source.extensionsUsed.size());
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
  // Bound source edges; hierarchy and geometry are never rebuilt here.
  for (const auto& node : source.nodes) b.refs(node.children.size());
  uint64_t roots{};
  for (const auto& scene : source.scenes) {
    charge(roots,scene.nodes.size(),limits.maxRootReferences,"scene root budget exceeded"); b.refs(scene.nodes.size());
  }
  if (source.scenes.empty()) {
    b.bytes(mul(source.nodes.size(),sizeof(int32_t)));
  }
  if(source.extensionsUsed.size()>limits.maxDeclarations || source.extensionsRequired.size()>limits.maxDeclarations)
    fail("extension declaration budget exceeded");
  for (const auto& name : source.extensionsUsed) {
    b.text(name);
    if (!knownDeclaration(name)) fail("unsupported extension declaration: " + name.substr(0,256));
  }
  for (size_t i=0;i<source.extensionsUsed.size();++i)
    if ((b.work(i+1), std::find(source.extensionsUsed.begin(),source.extensionsUsed.begin()+i,source.extensionsUsed[i]) !=
        source.extensionsUsed.begin()+i)) fail("duplicate extensionsUsed declaration");
  for (const auto& name : source.extensionsRequired) {
    b.text(name);
    b.work(source.extensionsUsed.size());
    if (std::ranges::find(source.extensionsUsed,name) == source.extensionsUsed.end()) fail("required extension is undeclared");
  }
  b.property(source.asset,{kGltfXmpExtension}); b.text(source.asset.version); b.text(source.asset.minVersion);
  b.text(source.asset.generator); b.text(source.asset.copyright);
  b.value(source.extras); b.text(source.extras_json_string); b.text(source.extensions_json_string);
  b.extensions(source.extensions,{"KHR_materials_variants","KHR_lights_punctual",kGltfXmpExtension,
      "KHR_lights_area","EXT_lights_area","WEBGI_lights_area","WEBGI_area_light"},false,true);
  for (const auto& buffer : source.buffers) {
    b.property(buffer); b.text(buffer.uri);
    if (buffer.data.empty() && !buffer.uri.empty()) fail("buffer is not resident");
    charge(b.payload,buffer.data.size(),limits.maxResidentBytes,"resident payload budget exceeded");
  }
  for (const auto& view : source.bufferViews) {
    b.property(view,{"EXT_meshopt_compression","KHR_meshopt_compression"}); index(view.buffer,source.buffers.size(),"buffer view buffer is invalid"); b.refs(1);
    const auto size = source.buffers[size_t(view.buffer)].data.size();
    if (view.byteOffset > size || view.byteLength > size-view.byteOffset) fail("buffer view range exceeds its buffer");
  }
  for (size_t a=0;a<source.accessors.size();++a) {
    const auto& accessor = source.accessors[a]; b.property(accessor);
    b.numbers(accessor.minValues); b.numbers(accessor.maxValues);
    const auto components=tinygltf::GetNumComponentsInType(uint32_t(accessor.type));
    if(components<=0) fail("accessor shape is invalid");
    charge(b.decoded,mul(mul(accessor.count,uint64_t(components)),sizeof(double)),limits.maxAccessorDecodedBytes,
        "accessor expansion budget exceeded");
    if (accessor.sparse.isSparse) {
      b.property(accessor.sparse); b.property(accessor.sparse.indices); b.property(accessor.sparse.values); b.refs(2);
      if(accessor.sparse.count<=0) fail("sparse accessor count is invalid");
      b.work(uint64_t(accessor.sparse.count));
    }
    const CheckedAccessor view(source,int(a),"material export compaction",{.allowFloat64=true});
    (void)view;
    b.refs(accessor.bufferView >= 0);
  }
  for (const auto& image : source.images) {
    b.property(image,{kGltfXmpExtension}); b.text(image.uri); b.text(image.mimeType);
    index(image.bufferView,source.bufferViews.size(),"image buffer view is invalid",true); b.refs(image.bufferView >= 0);
    if (image.image.empty() && image.bufferView < 0) fail("image is not resident");
    charge(b.payload,image.image.size(),limits.maxResidentBytes,"resident payload budget exceeded");
  }
  for (const auto& sampler : source.samplers) b.property(sampler);
  for (const auto& tex : source.textures) {
    b.property(tex,{"KHR_texture_basisu","EXT_texture_webp","EXT_texture_avif","MSFT_texture_dds"});
    index(tex.source,source.images.size(),"texture source is invalid",true);
    index(tex.sampler,source.samplers.size(),"texture sampler is invalid",true); b.refs(2);
    for(const auto& entry:tex.extensions) {
      const auto& body=entry.second;
      if(!body.Has("source") || !body.Get("source").IsInt()) fail("alternate image source is not an exact integer");
      index(body.Get("source").Get<int>(),source.images.size(),"alternate image source is invalid");b.refs(1);
    }
  }
  for (const auto& material : source.materials) {
    b.property(material,{"MSFT_lod",kGltfXmpExtension},true,false,true); b.numbers(material.lods); b.refs(material.lods.size());
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
    b.property(mesh,{kGltfXmpExtension}); b.numbers(mesh.weights);
    charge(b.primitives,mesh.primitives.size(),limits.maxPrimitives,"primitive count exceeds its bound");
    for (const auto& primitive : mesh.primitives) {
      b.property(primitive,{"KHR_materials_variants","KHR_draco_mesh_compression"});
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
    b.property(node,{"KHR_lights_punctual","EXT_mesh_gpu_instancing","KHR_node_visibility","KHR_node_selectability",kGltfXmpExtension,
        "KHR_node_hoverability","KHR_lights_area","EXT_lights_area","WEBGI_lights_area","WEBGI_area_light","MSFT_lod"});
    if (node.emitter != -1) fail("node audio reference schema is unsupported");
    b.numbers(node.lods); b.refs(node.lods.size());
    for (const auto lod : node.lods) index(lod,source.nodes.size(),"node LOD index is invalid");
    index(node.mesh,source.meshes.size(),"node mesh is invalid",true);
    index(node.skin,source.skins.size(),"node skin is invalid",true);
    index(node.camera,source.cameras.size(),"node camera is invalid",true);
    index(node.light,source.lights.size(),"node light is invalid",true); b.refs(4);
    for(const auto child:node.children) index(child,source.nodes.size(),"node child is invalid");
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
    b.property(animation,{kGltfXmpExtension}); b.table(animation.samplers); b.table(animation.channels);
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
      if(const auto found=channel.target_extensions.find("KHR_animation_pointer");found!=channel.target_extensions.end() &&
          found->second.IsObject() && found->second.Has("pointer") && found->second.Get("pointer").IsString())
        b.work(mul(found->second.Get("pointer").Get<std::string>().size(),3));
      (void)target(source,channel);
      const auto& sampler = animation.samplers[size_t(channel.sampler)];
      for (const auto id : {sampler.input,sampler.output}) {
        const auto& accessor=source.accessors[size_t(id)];
        const auto components=uint64_t(tinygltf::GetNumComponentsInType(uint32_t(accessor.type)));
        charge(b.animationDecoded,mul(mul(accessor.count,components),sizeof(double)),
            limits.maxAnimationDecodedBytes,"animation expansion budget exceeded");
      }
    }
  }
  for (const auto& scene : source.scenes) {
    b.property(scene,{kGltfXmpExtension}); b.numbers(scene.nodes);
    for(const auto root:scene.nodes) index(root,source.nodes.size(),"scene root is invalid");
    if (!scene.audioEmitters.empty()) fail("scene audio reference schema is unsupported");
  }
  index(source.defaultScene,source.scenes.size(),"default scene is invalid",true);
}

template<class ExtensionMap> auto* extension(ExtensionMap& map,const char* key) {
  const auto found=map.find(key);
  return found==map.end()?nullptr:&found->second;
}
template<class V> auto* member(V& value,const char* key) {
  if(!value.IsObject()) fail("reference container must be an object");
  auto& object=value.template Get<Object>();const auto found=object.find(key);
  return found==object.end()?nullptr:&found->second;
}
int exact(const Value& value,size_t count) {
  if(!value.IsInt()) fail("reference must be an exact integer");
  const auto id=value.Get<int>();index(id,count,"raw reference is outside its table");return id;
}
struct MaterialPointer {int32_t material{-1};size_t end{};};
MaterialPointer materialPointer(std::string_view path,size_t count) {
  if(path.empty() || path.front()!='/') fail("malformed animation pointer");
  for(size_t i=0;i<path.size();++i) if(path[i]=='~') {
    if(++i==path.size() || (path[i]!='0' && path[i]!='1')) fail("invalid JSON pointer escape");
  }
  constexpr std::string_view prefix="/materials/";
  if(!path.starts_with(prefix)) {
    if(path.starts_with("/nodes/") || path.starts_with("/cameras/") ||
        path.starts_with("/extensions/KHR_lights_punctual/lights/") ||
        path.starts_with("/extensions/KHR_lights_area/lights/") ||
        path.starts_with("/extensions/EXT_lights_area/lights/") ||
        path.starts_with("/extensions/WEBGI_lights_area/lights/") ||
        path.starts_with("/extensions/WEBGI_area_light/lights/")) return {};
    fail("unsupported animation pointer root may reference materials");
  }
  auto end=path.find('/',prefix.size());if(end==path.npos) end=path.size();
  const auto number=path.substr(prefix.size(),end-prefix.size());
  if(number.empty() || (number.size()>1 && number.front()=='0')) fail("malformed material pointer index");
  int32_t id{};const auto parsed=std::from_chars(number.data(),number.data()+number.size(),id);
  if(parsed.ec!=std::errc{} || parsed.ptr!=number.data()+number.size()) fail("malformed material pointer index");
  index(id,count,"material pointer is outside its table");return {id,end};
}

// The same deterministic walker counts preparation storage, captures exact
// before references, and collects writes. It never guesses from an 'index' key.
template<class Model,class Sink> void references(Model& source,std::span<const int32_t> map,Sink& sink) {
  const auto material=[&](auto& id,bool optional=false) {
    if(optional && id==-1) {sink.integer(id,-1);return false;}
    index(id,map.size(),"material reference is outside its table");
    const auto replacement=map[size_t(id)];
    if(replacement<0) fail("surviving reference targets a retired material");
    sink.integer(id,replacement);return replacement!=id;
  };
  for(const auto size:{source.materials.size(),source.meshes.size(),source.nodes.size(),source.skins.size(),
      source.scenes.size(),source.animations.size(),source.accessors.size(),source.bufferViews.size(),
      source.buffers.size(),source.images.size(),source.samplers.size(),source.textures.size(),
      source.cameras.size(),source.lights.size()}) sink.shape(size);
  size_t variantCount{};
  if(auto* root=extension(source.extensions,"KHR_materials_variants")) {
    const auto* variants=member(*root,"variants");
    if(!variants || !variants->IsArray()) fail("malformed variants table");
    variantCount=variants->template Get<Value::Array>().size();
    for(const auto& entry:variants->template Get<Value::Array>()) {
      const auto* name=member(entry,"name");if(!name || !name->IsString()) fail("variant has no name");
    }
  }
  sink.shape(variantCount);
  for(size_t m=0;m<source.materials.size();++m) {
    auto& row=source.materials[m];sink.shape(m);
    if(map[m]<0) continue; // This row's outgoing references disappear with it.
    sink.shape(row.lods.size());bool changed{};for(auto& id:row.lods) changed=material(id) || changed;
    auto* lod=extension(row.extensions,"MSFT_lod");sink.shape(lod!=nullptr);
    if(lod) {
      auto* ids=member(*lod,"ids");if(!ids || !ids->IsArray()) fail("malformed material LOD IDs");
      sink.shape(ids->template Get<Value::Array>().size());
      for(auto& value:ids->template Get<Value::Array>()) {
        (void)exact(value,map.size());changed=material(value.template Get<int>()) || changed;
      }
    }
    if(changed) sink.cache(row.extensions_json_string);
  }
  for(auto& mesh:source.meshes) {
    sink.shape(mesh.primitives.size());
    for(auto& primitive:mesh.primitives) {
      material(primitive.material,true);
      auto* variants=extension(primitive.extensions,"KHR_materials_variants");sink.shape(variants!=nullptr);
      if(!variants) continue;
      auto* mappings=member(*variants,"mappings");
      if(!mappings || !mappings->IsArray()) fail("malformed variant mappings");
      sink.shape(mappings->template Get<Value::Array>().size());bool changed{};
      for(auto& mapping:mappings->template Get<Value::Array>()) {
        auto* id=member(mapping,"material");const auto* variants=member(mapping,"variants");
        if(!id || !variants || !variants->IsArray() || variants->template Get<Value::Array>().empty())
          fail("malformed variant material binding");
        (void)exact(*id,map.size());changed=material(id->template Get<int>()) || changed;
        sink.shape(variants->template Get<Value::Array>().size());
        for(const auto& variant:variants->template Get<Value::Array>())
          sink.variant(exact(variant,variantCount));
      }
      if(changed) sink.cache(primitive.extensions_json_string);
    }
  }
  for(const auto& node:source.nodes) if(const auto* lod=extension(node.extensions,"MSFT_lod")) {
    const auto* ids=member(*lod,"ids");if(!ids || !ids->IsArray()) fail("malformed node LOD IDs");
    for(const auto& value:ids->template Get<Value::Array>()) (void)exact(value,source.nodes.size());
  }
  for(auto& animation:source.animations) {
    sink.shape(animation.channels.size());sink.shape(animation.samplers.size());
    for(auto& channel:animation.channels) {
      auto* pointer=extension(channel.target_extensions,"KHR_animation_pointer");sink.shape(pointer!=nullptr);
      if(!pointer) {if(channel.target_path=="pointer") fail("pointer channel has no pointer");continue;}
      auto* text=member(*pointer,"pointer");
      if(channel.target_path!="pointer" || channel.target_node!=-1 || !text || !text->IsString())
        fail("malformed pointer channel");
      auto& path=text->template Get<std::string>();const auto target=materialPointer(path,map.size());
      const auto replacement=target.material<0?-1:map[size_t(target.material)];
      if(target.material>=0 && replacement<0) fail("animation pointer targets a retired material");
      sink.pointer(path,target,replacement);
      if(target.material>=0 && target.material!=replacement) sink.cache(channel.target_extensions_json_string);
    }
  }
}

struct Counts {
  uint64_t integers{},shapes{},pointers{},pointerBytes{},caches{};
  void integer(const int&,int32_t) {++integers;}
  void shape(uint64_t) {++shapes;}
  void variant(int32_t) {++integers;}
  void pointer(const std::string& text,MaterialPointer,int32_t) {++pointers;pointerBytes=add(pointerBytes,text.size());}
  void cache(const std::string&) {++caches;}
};
struct Capture {
  std::vector<int32_t>& integers;std::vector<uint64_t>& shapes;std::vector<std::string>& pointers;
  void integer(const int& id,int32_t) {integers.push_back(id);}
  void shape(uint64_t size) {shapes.push_back(size);}
  void variant(int32_t id) {integers.push_back(id);}
  void pointer(const std::string& text,MaterialPointer,int32_t) {pointers.push_back(text);}
  void cache(const std::string&) {}
};
struct IntEdit {int* target{};int replacement{};};
struct StringEdit {std::string* target{};std::string replacement;};
struct Collect {
  const std::vector<int32_t>& integers;const std::vector<uint64_t>& shapes;const std::vector<std::string>& pointers;
  size_t integerAt{},shapeAt{},pointerAt{};
  std::vector<IntEdit> edits;std::vector<StringEdit> strings;std::vector<std::string*> caches;
  void match(int32_t id) {
    if(integerAt>=integers.size() || integers[integerAt++]!=id) fail("known references changed after planning");
  }
  void integer(int& id,int32_t replacement) {match(id);if(id!=replacement) edits.push_back({&id,replacement});}
  void shape(uint64_t size) {
    if(shapeAt>=shapes.size() || shapes[shapeAt++]!=size) fail("reference table shape changed after planning");
  }
  void variant(int32_t id) {match(id);}
  void pointer(std::string& text,MaterialPointer target,int32_t replacement) {
    if(pointerAt>=pointers.size() || pointers[pointerAt++]!=text) fail("animation pointer changed after planning");
    if(target.material>=0 && target.material!=replacement)
      strings.push_back({&text,"/materials/"+std::to_string(replacement)+text.substr(target.end)});
  }
  void cache(std::string& cache) {caches.push_back(&cache);}
  void finish() const {
    if(integerAt!=integers.size() || shapeAt!=shapes.size() || pointerAt!=pointers.size())
      fail("reference inventory changed after planning");
  }
};
bool validLimits(const GltfMaterialExportCompactionLimits& limits) {
  for(const auto value:{limits.maxTableEntries,limits.maxTotalObjects,limits.maxNodes,limits.maxScenes,
      limits.maxPrimitives,limits.maxReferences,limits.maxRootReferences,limits.maxAnimationChannels,
      limits.maxResidentBytes,limits.maxMetadataBytes,limits.maxAccessorDecodedBytes,limits.maxAnimationDecodedBytes,
      limits.maxOwnedBytes,limits.maxMetadataValues,limits.maxTraversalSteps})
    if(!value || value==UINT64_MAX) return false;
  return limits.maxMetadataDepth && limits.maxMetadataDepth<=64 && limits.maxDeclarations && limits.maxDeclarations<=1024;
}
} // namespace

struct GltfMaterialExportCompactionPlan::State {
  const tinygltf::Model* source{};
  GltfMaterialExportCompactionLimits limits;
  GltfMaterialExportCompactionStats stats;
  std::vector<int32_t> map,inverse,integers;
  std::vector<uint64_t> shapes;
  std::vector<std::string> pointers;
  Counts counts;
};
struct GltfMaterialExportCompactionAccess {
  using State=GltfMaterialExportCompactionPlan::State;
  static const State* state(const GltfMaterialExportCompactionPlan& plan) noexcept {return plan.state_.get();}
  static GltfMaterialExportCompactionPlan make(std::shared_ptr<const State> state) noexcept {
    GltfMaterialExportCompactionPlan result;result.state_=std::move(state);return result;
  }
};
GltfMaterialExportCompactionPlan::operator bool() const noexcept {return bool(state_);}
std::span<const int32_t> GltfMaterialExportCompactionPlan::oldToNewMaterials() const noexcept {
  return state_?std::span<const int32_t>(state_->map):std::span<const int32_t>{};
}
std::span<const int32_t> GltfMaterialExportCompactionPlan::newToOldMaterials() const noexcept {
  return state_?std::span<const int32_t>(state_->inverse):std::span<const int32_t>{};
}
GltfMaterialExportCompactionStats GltfMaterialExportCompactionPlan::stats() const noexcept {
  return state_?state_->stats:GltfMaterialExportCompactionStats{};
}

GltfMaterialExportCompactionPlan PlanGltfMaterialExportCompaction(const tinygltf::Model& source,
    std::span<const int32_t> retired,const GltfMaterialExportCompactionLimits& limits) {
  if(!validLimits(limits)) fail("preparation requires finite nonzero limits");
  if(source.materials.size()>limits.maxTableEntries || source.materials.size()>INT_MAX || retired.size()>source.materials.size())
    fail("material/retirement table exceeds its bound");
  uint64_t owned=add(sizeof(GltfMaterialExportCompactionAccess::State)+128,mul(source.materials.size(),8));
  if(owned>limits.maxOwnedBytes) fail("map storage budget exceeded");
  std::vector<int32_t> map(source.materials.size(),0),inverse;inverse.reserve(source.materials.size()-retired.size());
  for(const auto id:retired) {
    index(id,map.size(),"retired material is outside its table");
    if(map[size_t(id)]<0) fail("retired material is repeated");map[size_t(id)]=-1;
  }
  for(size_t id=0;id<map.size();++id) if(map[id]>=0) {map[id]=int32_t(inverse.size());inverse.push_back(int32_t(id));}
  Budget budget{limits,source};audit(source,budget);
  Counts counts;references(source,map,counts);
  budget.refs(add(counts.integers,counts.pointers));
  budget.work(mul(add(add(counts.integers,counts.shapes),counts.pointers),2));
  // Receipt plus Apply's simultaneously live edit/replacement storage. Pointer
  // construction is charged conservatively (two temporaries plus final string).
  owned=add(owned,add(mul(counts.integers,sizeof(int32_t)+sizeof(IntEdit)),mul(counts.shapes,sizeof(uint64_t))));
  owned=add(owned,add(mul(counts.pointers,sizeof(std::string)+sizeof(StringEdit)+64),mul(counts.pointerBytes,4)));
  owned=add(owned,mul(counts.caches,sizeof(std::string*)));
  if(owned>limits.maxOwnedBytes) fail("reference proof/edit workspace budget exceeded");
  auto state=std::make_shared<GltfMaterialExportCompactionAccess::State>();
  state->source=&source;state->limits=limits;state->map=std::move(map);state->inverse=std::move(inverse);state->counts=counts;
  state->stats={retired.size(),budget.references,budget.metadata,budget.payload,owned};
  state->integers.reserve(size_t(counts.integers));state->shapes.reserve(size_t(counts.shapes));state->pointers.reserve(size_t(counts.pointers));
  Capture capture{state->integers,state->shapes,state->pointers};references(source,state->map,capture);
  return GltfMaterialExportCompactionAccess::make(std::move(state));
}

void ApplyGltfMaterialExportCompaction(tinygltf::Model& source,const GltfMaterialExportCompactionPlan& plan) {
  const auto* state=GltfMaterialExportCompactionAccess::state(plan);
  if(!state || state->source!=&source || state->map.size()!=source.materials.size()) fail("stale or foreign export compaction plan");
  Budget budget{state->limits,source};audit(source,budget);
  budget.refs(add(state->counts.integers,state->counts.pointers));
  budget.work(mul(add(add(state->counts.integers,state->counts.shapes),state->counts.pointers),2));
  Collect collect{state->integers,state->shapes,state->pointers};
  collect.edits.reserve(size_t(state->counts.integers));collect.strings.reserve(size_t(state->counts.pointers));
  collect.caches.reserve(size_t(state->counts.caches));
  references(source,state->map,collect);collect.finish();
  // No throwing operations remain. Integer writes and string swaps precede
  // in-place survivor moves so addresses in the prepared edit list stay valid.
  static_assert(std::is_nothrow_move_assignable_v<tinygltf::Material>);
  for(const auto& edit:collect.edits) *edit.target=edit.replacement;
  for(auto& edit:collect.strings) edit.target->swap(edit.replacement);
  for(auto* cache:collect.caches) cache->clear();
  size_t kept{};
  for(size_t id=0;id<source.materials.size();++id) if(state->map[id]>=0) {
    if(kept!=id) source.materials[kept]=std::move(source.materials[id]);++kept;
  }
  source.materials.resize(kept);
}
} // namespace container::geometry::gltf
