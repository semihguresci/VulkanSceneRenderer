#include <Container/geometry/GltfResourceExportCompaction.h>
#include <Container/geometry/GltfMaterialTextureAuthoring.h>
#include <Container/geometry/GltfLegacyMaterialSerialization.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <climits>
#include <cmath>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace container::geometry::gltf {
namespace {
using Value=tinygltf::Value;
[[noreturn]] void fail(const char* text) {throw std::runtime_error(std::string("glTF resource export compaction: ")+text);}
uint64_t add(uint64_t a,uint64_t b) {if(b>UINT64_MAX-a) fail("budget overflow");return a+b;}
uint64_t mul(uint64_t a,uint64_t b) {if(a && b>UINT64_MAX/a) fail("budget overflow");return a*b;}
struct Budget {
  const GltfResourceExportCompactionLimits& limits;
  uint64_t bytes{},work{},payload{};
  void take(uint64_t n) {bytes=add(bytes,n);if(bytes>limits.maxOwnedBytes) fail("owned storage limit exceeded");}
  void visit(uint64_t n=1) {work=add(work,n);if(work>limits.maxTraversalSteps) fail("work limit exceeded");}
  void binary(uint64_t n) {payload=add(payload,n);if(payload>limits.maxPayloadBytes) fail("payload limit exceeded");}
  void text(const std::string& s) {take(add(s.size(),1));visit(s.size());}
  void value(const Value& v,unsigned depth=0) {
    if(depth>limits.references.maxMetadataDepth) fail("metadata depth exceeded");visit();take(sizeof(Value)+128);
    if((!v.IsString() && !v.Get<std::string>().empty()) || (!v.IsBinary() && !v.Get<std::vector<unsigned char>>().empty()) ||
        (!v.IsArray() && !v.Get<Value::Array>().empty()) || (!v.IsObject() && !v.Get<Value::Object>().empty())) fail("malformed inactive metadata storage");
    if(v.IsString()) text(v.Get<std::string>());
    else if(v.IsBinary()) {take(v.Get<std::vector<unsigned char>>().size());visit(v.Get<std::vector<unsigned char>>().size());}
    else if(v.IsArray()) for(const auto& x:v.Get<Value::Array>()) value(x,depth+1);
    else if(v.IsObject()) for(const auto& [key,x]:v.Get<Value::Object>()) {text(key);value(x,depth+1);}
    else if(v.IsNumber() && !std::isfinite(v.GetNumberAsDouble())) fail("nonfinite metadata");
  }
  template<class T> void numbers(const std::vector<T>& values) {take(mul(values.size(),sizeof(T)));visit(values.size());}
  template<class T> void property(const T& v) {
    visit();take(sizeof(T)+256);
#define TEXT(field) if constexpr(requires{v.field.size();v.field.c_str();}) text(v.field)
    TEXT(name);TEXT(uri);TEXT(mimeType);TEXT(extras_json_string);TEXT(extensions_json_string);
    TEXT(target_path);TEXT(target_extras_json_string);TEXT(target_extensions_json_string);
    TEXT(version);TEXT(minVersion);TEXT(generator);TEXT(copyright);TEXT(alphaMode);TEXT(type);TEXT(distanceModel);
#undef TEXT
    if constexpr(requires{v.extras;}) value(v.extras);
    if constexpr(requires{v.extensions;}) for(const auto& [key,x]:v.extensions) {text(key);value(x);}
    if constexpr(requires{v.target_extras;}) value(v.target_extras);
    if constexpr(requires{v.target_extensions;}) for(const auto& [key,x]:v.target_extensions) {text(key);value(x);}
#define NUMBERS(field) if constexpr(requires{numbers(v.field);}) numbers(v.field)
    NUMBERS(children);NUMBERS(matrix);NUMBERS(translation);NUMBERS(rotation);NUMBERS(scale);NUMBERS(weights);
    NUMBERS(joints);NUMBERS(nodes);NUMBERS(emissiveFactor);NUMBERS(baseColorFactor);NUMBERS(lods);
    NUMBERS(minValues);NUMBERS(maxValues);NUMBERS(color);NUMBERS(audioEmitters);
#undef NUMBERS
    if constexpr(requires{v.attributes;}) for(const auto& [key,x]:v.attributes) {text(key);take(sizeof(x)+64);visit();}
    if constexpr(requires{v.targets;}) {take(mul(v.targets.size(),sizeof(typename std::remove_cvref_t<decltype(v.targets)>::value_type)));for(const auto& target:v.targets) for(const auto& [key,x]:target) {text(key);take(sizeof(x)+64);visit();}}
    if constexpr(requires{v.pbrMetallicRoughness;}) {
      property(v.pbrMetallicRoughness);property(v.normalTexture);property(v.occlusionTexture);property(v.emissiveTexture);
      for(const auto* map:{&v.values,&v.additionalValues}) for(const auto& [key,p]:*map) {
        text(key);take(sizeof(p)+64);text(p.string_value);numbers(p.number_array);
        for(const auto& [k,n]:p.json_double_value) {text(k);take(sizeof(n)+64);visit();}
      }
    }
    if constexpr(requires{v.baseColorTexture;}) {property(v.baseColorTexture);property(v.metallicRoughnessTexture);}
#define CHILDREN(field) if constexpr(requires{v.field.begin();}) for(const auto& child:v.field) property(child)
    CHILDREN(primitives);CHILDREN(channels);CHILDREN(samplers);
#undef CHILDREN
    if constexpr(requires{v.perspective;}) {property(v.perspective);property(v.orthographic);}
    if constexpr(requires{v.spot;}) property(v.spot);
    if constexpr(requires{v.positional;}) property(v.positional);
    if constexpr(requires{v.sparse.isSparse;}) if(v.sparse.isSparse) {property(v.sparse);property(v.sparse.indices);property(v.sparse.values);}
  }
};
bool active(std::span<const uint8_t> mask,size_t row) {return mask.empty() || mask[row]!=0;}
void mask(std::span<const uint8_t> values,size_t n,const GltfResourceExportCompactionLimits& limits) {
  if(n>limits.maxTableEntries || n>INT_MAX || (!values.empty() && values.size()!=n) ||
      !std::ranges::all_of(values,[](uint8_t x){return x<=1;})) fail("phase mask/table is invalid");
}
struct Masks {std::vector<uint8_t> images,textures,samplers,materials,nodes;};
GltfResourceExportMasks view(const Masks& masks) {return {masks.images,masks.textures,masks.samplers,masks.materials,masks.nodes};}
struct Pointer {enum Kind {Other,Image,Texture,Sampler,Material,Node} kind{Other};uint32_t row{};size_t end{};};
Pointer pointer(const tinygltf::AnimationChannel& channel) {
  if(channel.target_path!="pointer") return {Pointer::Node,uint32_t(channel.target_node),0};
  const auto ext=channel.target_extensions.find("KHR_animation_pointer");
  if(ext==channel.target_extensions.end() || !ext->second.IsObject()) fail("malformed animation pointer");
  const auto& object=ext->second.Get<Value::Object>();const auto p=object.find("pointer");
  if(p==object.end() || !p->second.IsString()) fail("malformed animation pointer");
  const auto& text=p->second.Get<std::string>();
  for(const auto [prefix,kind]:{std::pair<std::string_view,Pointer::Kind>{"/images/",Pointer::Image},
      {"/textures/",Pointer::Texture},{"/samplers/",Pointer::Sampler},{"/materials/",Pointer::Material},{"/nodes/",Pointer::Node}}) {
    if(!text.starts_with(prefix)) continue;
    const auto end=text.find('/',prefix.size());const auto stop=end==std::string::npos?text.size():end;
    const auto digits=std::string_view(text).substr(prefix.size(),stop-prefix.size());uint32_t row{};
    const auto parsed=std::from_chars(digits.data(),digits.data()+digits.size(),row);
    if(digits.empty() || (digits.size()>1 && digits.front()=='0') || parsed.ec!=std::errc{} || parsed.ptr!=digits.data()+digits.size()) fail("malformed animation pointer index");
    return {kind,row,stop};
  }
  return {};
}
bool keepChannel(const tinygltf::AnimationChannel& channel,const tinygltf::Model& source,GltfResourceExportMasks masks) {
  const auto target=pointer(channel);
  if(target.kind==Pointer::Node) {if(target.row>=source.nodes.size()) fail("animation node target is invalid");return active(masks.nodes,target.row);}
  if(target.kind==Pointer::Material) {if(target.row>=source.materials.size()) fail("animation material target is invalid");return active(masks.materials,target.row);}
  return true;
}
template<class T> void copyProperty(T& to,const T& from) {
  to.name=from.name;to.extras=from.extras;to.extensions=from.extensions;
  to.extras_json_string=from.extras_json_string;to.extensions_json_string=from.extensions_json_string;
}
template<class Table> void costTable(Budget& budget,const Table& rows) {
  if(rows.size()>budget.limits.maxTableEntries) fail("table count exceeded");
  for(const auto& row:rows) budget.property(row);
}
void projection(std::unique_ptr<tinygltf::Model>& holder,const tinygltf::Model& source,GltfResourceExportMasks masks,Budget& budget) {
  // Preflight every copied metadata/shape allocation before constructing the
  // detached reference projection. Binary buffers and image pixels are borrowed
  // only through exact storage identities; they are never copied here.
  if(source.buffers.size()>budget.limits.maxTableEntries || source.animations.size()>budget.limits.maxTableEntries) fail("table count exceeded");
  budget.take(mul(source.images.size(),sizeof(tinygltf::Image)+512));
  budget.take(mul(source.textures.size(),sizeof(tinygltf::Texture)+512));
  budget.take(mul(source.samplers.size(),sizeof(tinygltf::Sampler)+512));
  budget.property(source.asset);budget.take(sizeof(source)+128);budget.value(source.extras);
  budget.text(source.extras_json_string);budget.text(source.extensions_json_string);
  for(const auto& [name,value]:source.extensions) {budget.text(name);budget.value(value);}
  for(const auto* names:{&source.extensionsUsed,&source.extensionsRequired}) for(const auto& name:*names) budget.text(name);
  for(const auto& buffer:source.buffers) {budget.property(buffer);budget.binary(buffer.data.size());}
  for(size_t i=0;i<source.images.size();++i) if(active(masks.images,i)) {budget.property(source.images[i]);budget.binary(source.images[i].image.size());}
#define COST(field) costTable(budget,source.field)
  COST(bufferViews);COST(accessors);COST(meshes);COST(skins);COST(cameras);COST(lights);COST(scenes);COST(audioEmitters);COST(audioSources);
#undef COST
  for(size_t i=0;i<source.samplers.size();++i) if(active(masks.samplers,i)) budget.property(source.samplers[i]);
  for(size_t i=0;i<source.textures.size();++i) if(active(masks.textures,i)) budget.property(source.textures[i]);
  for(size_t i=0;i<source.materials.size();++i) if(active(masks.materials,i)) budget.property(source.materials[i]);
    else {budget.take(sizeof(tinygltf::Material)+2048);budget.text(source.materials[i].name);budget.value(source.materials[i].extras);}
  for(size_t i=0;i<source.nodes.size();++i) if(active(masks.nodes,i)) budget.property(source.nodes[i]);
    else {budget.take(sizeof(tinygltf::Node)+512);budget.text(source.nodes[i].name);budget.value(source.nodes[i].extras);budget.numbers(source.nodes[i].children);}
  for(const auto& animation:source.animations) {
    budget.take(sizeof(animation)+64);budget.text(animation.name);budget.value(animation.extras);
    for(const auto& [name,value]:animation.extensions) {budget.text(name);budget.value(value);}
    budget.text(animation.extras_json_string);budget.text(animation.extensions_json_string);
    costTable(budget,animation.samplers);
    budget.take(mul(animation.channels.size(),sizeof(tinygltf::AnimationChannel)+128));
    for(const auto& channel:animation.channels) {budget.visit();if(keepChannel(channel,source,masks)) budget.property(channel);}
  }
  holder=std::make_unique<tinygltf::Model>();auto& output=*holder;output.asset=source.asset;output.extensions=source.extensions;output.extras=source.extras;
  output.extras_json_string=source.extras_json_string;output.extensions_json_string=source.extensions_json_string;
  output.extensionsUsed=source.extensionsUsed;output.extensionsRequired=source.extensionsRequired;output.defaultScene=source.defaultScene;
#define COPY(field) output.field=source.field
  COPY(bufferViews);COPY(accessors);COPY(meshes);COPY(skins);COPY(cameras);COPY(lights);COPY(scenes);COPY(audioEmitters);COPY(audioSources);
#undef COPY
  output.buffers.resize(source.buffers.size());for(size_t i=0;i<source.buffers.size();++i) {copyProperty(output.buffers[i],source.buffers[i]);output.buffers[i].uri=source.buffers[i].uri;}
  output.images.resize(source.images.size());for(size_t i=0;i<source.images.size();++i) if(active(masks.images,i)) {
    const auto& old=source.images[i];auto& next=output.images[i];copyProperty(next,old);
    next.width=old.width;next.height=old.height;next.component=old.component;next.bits=old.bits;next.pixel_type=old.pixel_type;
    next.bufferView=old.bufferView;next.mimeType=old.mimeType;next.uri=old.uri;next.as_is=old.as_is;
  }
  output.samplers.resize(source.samplers.size());for(size_t i=0;i<source.samplers.size();++i) if(active(masks.samplers,i)) output.samplers[i]=source.samplers[i];
  output.textures.resize(source.textures.size());for(size_t i=0;i<source.textures.size();++i) if(active(masks.textures,i)) output.textures[i]=source.textures[i];
  output.materials.resize(source.materials.size());for(size_t i=0;i<source.materials.size();++i) {
    if(active(masks.materials,i)) output.materials[i]=source.materials[i];
    else {output.materials[i].name=source.materials[i].name;output.materials[i].extras=source.materials[i].extras;}
  }
  output.nodes.resize(source.nodes.size());for(size_t i=0;i<source.nodes.size();++i) {
    if(active(masks.nodes,i)) output.nodes[i]=source.nodes[i];
    else {output.nodes[i].name=source.nodes[i].name;output.nodes[i].extras=source.nodes[i].extras;output.nodes[i].children=source.nodes[i].children;}
  }
  output.animations.resize(source.animations.size());for(size_t i=0;i<source.animations.size();++i) {
    const auto& old=source.animations[i];auto& next=output.animations[i];copyProperty(next,old);next.samplers=old.samplers;
    next.channels.reserve(old.channels.size());for(const auto& channel:old.channels) if(keepChannel(channel,source,masks)) next.channels.push_back(channel);
  }
}
struct Storage {const void* data{};size_t size{};bool operator==(const Storage&) const=default;};
std::vector<Storage> storage(const tinygltf::Model& model,Budget& budget) {
  budget.take(mul(add(model.buffers.size(),model.images.size()),sizeof(Storage)));
  std::vector<Storage> result;result.reserve(model.buffers.size()+model.images.size());
  for(const auto& buffer:model.buffers) result.push_back({buffer.data.data(),buffer.data.size()});
  for(const auto& image:model.images) result.push_back({image.image.data(),image.image.size()});
  return result;
}
void maps(size_t n,std::span<const uint8_t> mask,std::vector<int32_t>& oldToNew,std::vector<int32_t>& newToOld,Budget& budget) {
  budget.take(mul(n,2*sizeof(int32_t)));oldToNew.assign(n,-1);newToOld.reserve(n);
  for(size_t i=0;i<n;++i) if(active(mask,i)) {oldToNew[i]=int32_t(newToOld.size());newToOld.push_back(int32_t(i));}
}
int remap(int id,const std::vector<int32_t>& map,bool optional=true) {
  if(optional && id==-1) return -1;
  if(id<0 || size_t(id)>=map.size() || map[size_t(id)]<0) fail("active owner references a removed/missing resource");
  return map[size_t(id)];
}
bool knownLegacyTexture(std::string_view name) {
  constexpr std::array<std::string_view,5> core{"baseColorTexture","metallicRoughnessTexture","normalTexture","occlusionTexture","emissiveTexture"};
  return std::ranges::find(core,name)!=core.end() || std::ranges::find(KnownLegacyMaterialTextureFields(),name)!=KnownLegacyMaterialTextureFields().end();
}
void remapMaterial(tinygltf::Material& material,const std::vector<int32_t>& textures) {
  for(auto* info:{&material.pbrMetallicRoughness.baseColorTexture,&material.pbrMetallicRoughness.metallicRoughnessTexture,&material.emissiveTexture}) info->index=remap(info->index,textures);
  material.normalTexture.index=remap(material.normalTexture.index,textures);material.occlusionTexture.index=remap(material.occlusionTexture.index,textures);
  bool extensionChanged=false;
  const auto rewrite=[&](std::string_view extension,std::string_view field) {
    auto e=material.extensions.find(std::string(extension));if(e==material.extensions.end()) return;
    auto& object=e->second.Get<Value::Object>();auto f=object.find(std::string(field));if(f==object.end()) return;
    auto& index=f->second.Get<Value::Object>().at("index").Get<int>();const auto next=remap(index,textures,false);extensionChanged|=next!=index;index=next;
  };
  for(const auto& slot:GltfMaterialTextureSlotRegistry()) if(!slot.extension.empty()) rewrite(slot.extension,slot.path.substr(slot.path.rfind('/')+1));
  rewrite("KHR_materials_pbrSpecularGlossiness","diffuseTexture");rewrite("KHR_materials_pbrSpecularGlossiness","specularGlossinessTexture");
  for(auto* values:{&material.values,&material.additionalValues}) for(auto& [name,parameter]:*values) if(knownLegacyTexture(name)) {
    auto& index=parameter.json_double_value.at("index");index=remap(int(index),textures,false);
  }
  if(extensionChanged) material.extensions_json_string.clear();
}
}

struct GltfResourceExportCompactionPlan::State {
  const tinygltf::Model* identity{};
  Masks masks;
  std::vector<int32_t> images,textures,samplers,imageSources,textureSources,samplerSources;
  std::vector<Storage> storage;
  std::unique_ptr<tinygltf::Model> metadata;
  GltfTextureResourceReferences proof;
  GltfResourceExportCompactionLimits limits;
  GltfResourceExportCompactionStats stats;
};
struct GltfResourceExportCompactionAccess {
  static const auto& get(const GltfResourceExportCompactionPlan& p) {return p.state_;}
  static GltfResourceExportCompactionPlan make(std::shared_ptr<GltfResourceExportCompactionPlan::State> p) {GltfResourceExportCompactionPlan result;result.state_=std::move(p);return result;}
  using State=GltfResourceExportCompactionPlan::State;
};
GltfResourceExportCompactionPlan::operator bool() const noexcept {return bool(state_);}
#define MAP(method,field) std::span<const int32_t> GltfResourceExportCompactionPlan::method() const noexcept {return state_?std::span<const int32_t>(state_->field):std::span<const int32_t>{};}
MAP(oldToNewImages,images) MAP(oldToNewTextures,textures) MAP(oldToNewSamplers,samplers)
MAP(newToOldImages,imageSources) MAP(newToOldTextures,textureSources) MAP(newToOldSamplers,samplerSources)
#undef MAP
GltfResourceExportCompactionStats GltfResourceExportCompactionPlan::stats() const noexcept {return state_?state_->stats:GltfResourceExportCompactionStats{};}

GltfResourceExportCompactionPlan PlanGltfResourceExportCompaction(const tinygltf::Model& model,GltfResourceExportMasks masks,
    const GltfResourceExportCompactionLimits& limits) {
  for(const auto n:{limits.maxTableEntries,limits.maxOwnedBytes,limits.maxPayloadBytes,limits.maxTraversalSteps}) if(!n || n==UINT64_MAX) fail("limits must be finite and positive");
  if(!limits.references.maxMetadataDepth || limits.references.maxMetadataDepth>256) fail("metadata depth capacity is invalid");
  mask(masks.images,model.images.size(),limits);mask(masks.textures,model.textures.size(),limits);mask(masks.samplers,model.samplers.size(),limits);
  mask(masks.materials,model.materials.size(),limits);mask(masks.nodes,model.nodes.size(),limits);
  Budget budget{limits};budget.take(sizeof(GltfResourceExportCompactionAccess::State)+128);
  budget.visit(add(add(masks.images.size(),masks.textures.size()),add(add(masks.samplers.size(),masks.materials.size()),masks.nodes.size())));
  std::unique_ptr<tinygltf::Model> holder;projection(holder,model,masks,budget);const auto& projected=*holder;
  auto refs=limits.references;refs.policy=GltfTextureResourceReferencePolicy::RequireComplete;
  refs.maxOwnedBytes=std::min(refs.maxOwnedBytes,limits.maxOwnedBytes-budget.bytes);
  auto proof=ExtractGltfTextureResourceReferences(projected,{masks.materials,masks.textures,masks.images,masks.samplers},refs);
  budget.take(proof.stats().ownedBytes);budget.visit(proof.stats().traversalSteps);
  auto state=std::make_shared<GltfResourceExportCompactionAccess::State>();state->identity=&model;state->limits=limits;state->proof=std::move(proof);state->metadata=std::move(holder);
  const auto copy=[&](auto input,auto& target) {budget.take(input.size());if(!input.empty()) target.assign(input.begin(),input.end());};
  copy(masks.images,state->masks.images);copy(masks.textures,state->masks.textures);copy(masks.samplers,state->masks.samplers);
  copy(masks.materials,state->masks.materials);copy(masks.nodes,state->masks.nodes);
  maps(model.images.size(),masks.images,state->images,state->imageSources,budget);maps(model.textures.size(),masks.textures,state->textures,state->textureSources,budget);
  maps(model.samplers.size(),masks.samplers,state->samplers,state->samplerSources,budget);state->storage=storage(model,budget);
  state->stats={model.images.size()-state->imageSources.size(),model.textures.size()-state->textureSources.size(),model.samplers.size()-state->samplerSources.size(),
      state->proof.stats().references,budget.bytes,budget.payload};return GltfResourceExportCompactionAccess::make(std::move(state));
}

void ApplyGltfResourceExportCompaction(tinygltf::Model& model,const GltfResourceExportCompactionPlan& plan) {
  const auto& owner=GltfResourceExportCompactionAccess::get(plan);if(!owner || owner->identity!=&model) fail("plan belongs to another model");
  const auto& state=*owner;const auto masks=view(state.masks);Budget budget{state.limits};budget.take(state.stats.ownedBytes);
  if(model.images.size()!=state.images.size() || model.textures.size()!=state.textures.size() || model.samplers.size()!=state.samplers.size()) fail("resource table shape changed");
  mask(masks.materials,model.materials.size(),state.limits);mask(masks.nodes,model.nodes.size(),state.limits);
  std::unique_ptr<tinygltf::Model> holder;projection(holder,model,masks,budget);auto& projected=*holder;
  if(storage(model,budget)!=state.storage || projected!=*state.metadata ||
      !GltfTextureResourceReferencesMatch(projected,{masks.materials,masks.textures,masks.images,masks.samplers},state.proof)) fail("model reference/storage witness changed");
  for(auto& material:projected.materials) remapMaterial(material,state.textures);
  for(const auto old:state.textureSources) {
    auto& texture=projected.textures[size_t(old)];texture.source=remap(texture.source,state.images);texture.sampler=remap(texture.sampler,state.samplers);
    bool changed=false;
    for(const auto name:{"KHR_texture_basisu","EXT_texture_webp","MSFT_texture_dds","EXT_texture_avif"}) {
      auto extension=texture.extensions.find(name);if(extension==texture.extensions.end()) continue;
      auto& index=extension->second.Get<Value::Object>().at("source").Get<int>();const auto next=remap(index,state.images,false);changed|=next!=index;index=next;
    }
    if(changed) texture.extensions_json_string.clear();
  }
  // The strict reference proof rejects unsupported direct resource animation
  // domains today. Keep this explicit prefix mapping paired with that schema so
  // future supported non-index resource properties cannot silently keep old IDs.
  for(auto& animation:projected.animations) for(auto& channel:animation.channels) {
    const auto target=pointer(channel);const std::vector<int32_t>* map=nullptr;
    if(target.kind==Pointer::Image) map=&state.images;else if(target.kind==Pointer::Texture) map=&state.textures;else if(target.kind==Pointer::Sampler) map=&state.samplers;
    if(!map) continue;const auto next=remap(int(target.row),*map,false);
    auto& text=channel.target_extensions.at("KHR_animation_pointer").Get<Value::Object>().at("pointer").Get<std::string>();
    const auto start=text.find('/',1)+1;text=text.substr(0,start)+std::to_string(next)+text.substr(target.end);channel.target_extensions_json_string.clear();
  }
  budget.take(mul(state.imageSources.size(),sizeof(tinygltf::Image)));budget.take(mul(state.textureSources.size(),sizeof(tinygltf::Texture)));budget.take(mul(state.samplerSources.size(),sizeof(tinygltf::Sampler)));
  std::vector<tinygltf::Image> images(state.imageSources.size());std::vector<tinygltf::Texture> textures(state.textureSources.size());std::vector<tinygltf::Sampler> samplers(state.samplerSources.size());
  for(size_t i=0;i<textures.size();++i) textures[i]=std::move(projected.textures[size_t(state.textureSources[i])]);
  for(size_t i=0;i<samplers.size();++i) samplers[i]=std::move(projected.samplers[size_t(state.samplerSources[i])]);
  static_assert(std::is_nothrow_move_assignable_v<tinygltf::Image> && std::is_nothrow_move_assignable_v<tinygltf::Node>);
  // All allocation/validation precedes the first source edit. Image move
  // assignment transfers its existing pixel allocation without copying it.
  for(size_t i=0;i<images.size();++i) images[i]=std::move(model.images[size_t(state.imageSources[i])]);
  for(size_t i=0;i<model.nodes.size();++i) if(!active(masks.nodes,i)) model.nodes[i]=std::move(projected.nodes[i]);
  model.images.swap(images);model.textures.swap(textures);model.samplers.swap(samplers);
  model.materials.swap(projected.materials);model.animations.swap(projected.animations);
}
} // namespace container::geometry::gltf
