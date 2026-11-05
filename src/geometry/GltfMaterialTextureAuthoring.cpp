#include <Container/geometry/GltfMaterialTextureAuthoring.h>

#include <algorithm>
#include <bit>
#include <climits>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace container::geometry::gltf {
namespace {
using S=GltfMaterialTextureSlot;
using V=tinygltf::Value;
using O=V::Object;
constexpr std::string_view transformName="KHR_texture_transform",sgName="KHR_materials_pbrSpecularGlossiness";
constexpr std::array<GltfMaterialTextureSlotInfo,size_t(S::Count)> slots{{
 {S::BaseColor,"Base color","pbrMetallicRoughness/baseColorTexture",{},true},
 {S::Normal,"Normal","normalTexture",{}}, {S::Occlusion,"Occlusion","occlusionTexture",{}},
 {S::Emissive,"Emissive","emissiveTexture",{},true},
 {S::MetallicRoughness,"Metallic / roughness","pbrMetallicRoughness/metallicRoughnessTexture",{}},
 {S::Roughness,"Roughness","roughnessTexture",{}}, {S::Metalness,"Metalness","metalnessTexture",{}},
 {S::Specular,"Specular","extensions/KHR_materials_specular/specularTexture","KHR_materials_specular"},
 {S::Height,"Height","heightTexture",{}}, {S::Opacity,"Opacity","opacityTexture",{}},
 {S::Transmission,"Transmission","extensions/KHR_materials_transmission/transmissionTexture","KHR_materials_transmission"},
 {S::SpecularColor,"Specular color","extensions/KHR_materials_specular/specularColorTexture","KHR_materials_specular",true},
 {S::Clearcoat,"Clearcoat","extensions/KHR_materials_clearcoat/clearcoatTexture","KHR_materials_clearcoat"},
 {S::ClearcoatRoughness,"Clearcoat roughness","extensions/KHR_materials_clearcoat/clearcoatRoughnessTexture","KHR_materials_clearcoat"},
 {S::ClearcoatNormal,"Clearcoat normal","extensions/KHR_materials_clearcoat/clearcoatNormalTexture","KHR_materials_clearcoat"},
 {S::Thickness,"Thickness","extensions/KHR_materials_volume/thicknessTexture","KHR_materials_volume"},
 {S::SheenColor,"Sheen color","extensions/KHR_materials_sheen/sheenColorTexture","KHR_materials_sheen",true},
 {S::SheenRoughness,"Sheen roughness","extensions/KHR_materials_sheen/sheenRoughnessTexture","KHR_materials_sheen"},
 {S::Iridescence,"Iridescence","extensions/KHR_materials_iridescence/iridescenceTexture","KHR_materials_iridescence"},
 {S::IridescenceThickness,"Iridescence thickness","extensions/KHR_materials_iridescence/iridescenceThicknessTexture","KHR_materials_iridescence"},
 {S::Anisotropy,"Anisotropy","extensions/KHR_materials_anisotropy/anisotropyTexture","KHR_materials_anisotropy"},
 {S::DiffuseTransmission,"Diffuse transmission","extensions/KHR_materials_diffuse_transmission/diffuseTransmissionTexture","KHR_materials_diffuse_transmission",false,3},
 {S::DiffuseTransmissionColor,"Diffuse transmission color","extensions/KHR_materials_diffuse_transmission/diffuseTransmissionColorTexture","KHR_materials_diffuse_transmission",true}
}};
[[noreturn]] void fail(std::string_view text) {throw std::invalid_argument("glTF material texture authoring: "+std::string(text));}
std::string key(std::string_view text) {return std::string(text);}
bool exact(const V& a,const V& b) {
 if(a.Type()!=b.Type()) return false;
 if(a.IsInt()) return a.Get<int>()==b.Get<int>();
 if(a.IsReal()) return std::bit_cast<uint64_t>(a.Get<double>())==std::bit_cast<uint64_t>(b.Get<double>());
 if(a.IsBool()) return a.Get<bool>()==b.Get<bool>();
 if(a.IsString()) return a.Get<std::string>()==b.Get<std::string>();
 if(a.IsBinary()) return a.Get<std::vector<unsigned char>>()==b.Get<std::vector<unsigned char>>();
 if(a.IsArray()) {if(a.ArrayLen()!=b.ArrayLen()) return false;for(size_t i=0;i<a.ArrayLen();++i) if(!exact(a.Get(int(i)),b.Get(int(i)))) return false;}
 if(a.IsObject()) {const auto& x=a.Get<O>();const auto& y=b.Get<O>();if(x.size()!=y.size()) return false;auto j=y.begin();for(const auto& [name,value]:x) {if(name!=j->first || !exact(value,j->second)) return false;++j;}}
 return true;
}
template<class Map> bool exactMap(const Map& a,const Map& b) {
 if(a.size()!=b.size()) return false;auto j=b.begin();for(const auto& [name,value]:a) {if(name!=j->first || !exact(value,j->second)) return false;++j;}return true;
}
const V* member(const O& object,std::string_view name) {const auto i=object.find(key(name));return i==object.end()?nullptr:&i->second;}
const O* objectMember(const O& object,std::string_view name) {const auto* value=member(object,name);if(!value) return nullptr;if(!value->IsObject()) fail("selected texture object must be an object");return &value->Get<O>();}
bool put(O& object,std::string_view name,V value) {const auto found=object.find(key(name));if(found!=object.end() && exact(found->second,value)) return false;object[key(name)]=std::move(value);return true;}
bool remove(O& object,std::string_view name) {return object.erase(key(name))!=0;}
bool copyMember(O& target,const O* source,std::string_view name) {if(source) if(const auto* value=member(*source,name)) return put(target,name,*value);return remove(target,name);}
O& ensure(O& parent,std::string_view name) {auto i=parent.find(key(name));if(i==parent.end()) i=parent.emplace(key(name),V(O{})).first;if(!i->second.IsObject()) fail("selected texture container must be an object");return i->second.Get<O>();}
const O* ext(const tinygltf::Material& m,std::string_view name) {return objectMember(m.extensions,name);}
bool isSg(const tinygltf::Material& m) {return m.extensions.contains(key(sgName));}
std::array<std::string_view,2> aliases(S slot) {
 switch(slot) {
  case S::Roughness:return {"roughnessTexture",{}};case S::Metalness:return {"metalnessTexture",{}};
  case S::Specular:return {"specularTexture",{}};case S::Height:return {"heightTexture","displacementTexture"};
  case S::Opacity:return {"opacityTexture",{}};case S::Transmission:return {"refractionTexture","transmissionTexture"};
  default:return {};
 }
}
// TinyGLTF also retains core texture infos in ParameterMaps. These are raw
// mirrors of the typed core fields, not another effective-binding fallback.
std::string_view coreAlias(S slot) {
 switch(slot) {
  case S::BaseColor:return "baseColorTexture";
  case S::Normal:return "normalTexture";
  case S::Occlusion:return "occlusionTexture";
  case S::Emissive:return "emissiveTexture";
  case S::MetallicRoughness:return "metallicRoughnessTexture";
  default:return {};
 }
}
struct Location {S slot;std::string_view extension,field,path;bool legacy{};bool additional{};std::optional<S> coupled;};
Location primary(const tinygltf::Material& m,S slot) {
 const auto& d=GltfMaterialTextureSlotDescription(slot);
 Location result{slot,d.extension,d.path.substr(d.path.rfind('/')==std::string_view::npos?0:d.path.rfind('/')+1),d.path};
 if(isSg(m)) {
  if(slot==S::BaseColor) result={slot,sgName,"diffuseTexture","extensions/KHR_materials_pbrSpecularGlossiness/diffuseTexture"};
  else if(slot==S::Roughness || slot==S::SpecularColor)
   result={slot,sgName,"specularGlossinessTexture","extensions/KHR_materials_pbrSpecularGlossiness/specularGlossinessTexture",false,false,slot==S::Roughness?S::SpecularColor:S::Roughness};
 }
 if(result.extension.empty() && (slot==S::Roughness || slot==S::Metalness || slot==S::Height || slot==S::Opacity)) result.legacy=true;
 return result;
}
template<class Material,class Function> bool core(Material& m,S slot,Function&& function) {
 switch(slot) {
  case S::BaseColor:if(!isSg(m)) {function(m.pbrMetallicRoughness.baseColorTexture);return true;}break;
  case S::Normal:function(m.normalTexture);return true;
  case S::Occlusion:function(m.occlusionTexture);return true;
  case S::Emissive:function(m.emissiveTexture);return true;
  case S::MetallicRoughness:function(m.pbrMetallicRoughness.metallicRoughnessTexture);return true;
  default:break;
 }return false;
}
int index(const O* info) {
 if(!info) return -1;const auto* value=member(*info,"index");if(!value) return -1;
 if(!value->IsInt() || value->Get<int>()<0) fail("selected source texture index must be a nonnegative integer");return value->Get<int>();
}
const O* info(const tinygltf::Material& m,const Location& at) {
 const auto* parent=ext(m,at.extension);return parent?objectMember(*parent,at.field):nullptr;
}
const tinygltf::Parameter* parameter(const tinygltf::Material& m,const Location& at) {
 const auto& map=at.additional?m.additionalValues:m.values;const auto i=map.find(key(at.field));return i==map.end()?nullptr:&i->second;
}
Location locate(const tinygltf::Material& m,S slot) {
 auto result=primary(m,slot);int bound=-1;
 const bool typed=core(m,slot,[&](const auto& value) {bound=value.index;});
 if(!typed && !result.legacy) bound=index(info(m,result));
 if(bound>=0) return result;
 for(const auto name:aliases(slot)) if(!name.empty()) for(bool additional:{false,true}) {
  const auto& map=additional?m.additionalValues:m.values;
  if(map.contains(key(name))) return {slot,{},name,name,true,additional,{}};
 }
 return result;
}
bool sameField(const Location& a,const Location& b) {return a.legacy==b.legacy && a.extension==b.extension && a.field==b.field && (!a.legacy || a.additional==b.additional);}
double finite(double value) {if(!std::isfinite(value) || std::abs(value)>std::numeric_limits<float>::max()) fail("mapping component must be finite and representable as binary32");return value;}
uint32_t uintNumber(double value,uint32_t maximum) {if(!std::isfinite(value) || value<0 || value>maximum || std::trunc(value)!=value) fail("texture mapping integer is outside its supported range");return uint32_t(value);}
void readTransform(GltfMaterialTextureBinding& out,const O* transform) {
 if(!transform) return;
 if(const auto* value=member(*transform,"texCoord")) {if(!value->IsInt()) fail("texture transform texCoord must be integer");out.texCoord=uintNumber(value->GetNumberAsDouble(),1);}
 if(const auto* value=member(*transform,"rotation")) {if(!value->IsNumber()) fail("texture rotation must be numeric");out.rotation=finite(value->GetNumberAsDouble());}
 for(const auto [name,destination]:{std::pair{"offset",&out.offset},std::pair{"scale",&out.scale}}) if(const auto* value=member(*transform,name)) {
  if(!value->IsArray() || value->ArrayLen()!=2) fail("mapping vector must have two components");
  for(int i=0;i<2;++i) {if(!value->Get(i).IsNumber()) fail("mapping vector component must be numeric");(*destination)[size_t(i)]=finite(value->Get(i).GetNumberAsDouble());}
 }
}
GltfMaterialTextureBinding read(const tinygltf::Material& m,const Location& at) {
 GltfMaterialTextureBinding result;result.path=at.path;result.extension=at.extension;result.legacy=at.legacy;
 result.channelEditable=at.legacy;result.channel=GltfMaterialTextureSlotDescription(at.slot).channel;result.coupledSlot=at.coupled;
 if(at.legacy) {
  const auto* p=parameter(m,at);if(!p) return result;
  const auto& numbers=p->json_double_value;
  const auto source=numbers.find("index");if(source==numbers.end()) fail("legacy texture has no index");result.index=int32_t(uintNumber(source->second,INT_MAX));
  if(const auto i=numbers.find("texCoord");i!=numbers.end()) result.texCoord=uintNumber(i->second,1);
  if(const auto i=numbers.find("channel");i!=numbers.end()) result.channel=uintNumber(i->second,4);
  for(const auto [name,destination]:{std::pair{"offsetX",&result.offset[0]},std::pair{"offsetY",&result.offset[1]},
      std::pair{"scaleX",&result.scale[0]},std::pair{"scaleY",&result.scale[1]},std::pair{"rotation",&result.rotation}})
   if(const auto i=numbers.find(name);i!=numbers.end()) *destination=finite(i->second);
  return result;
 }
 if(core(m,at.slot,[&](const auto& value) {
  result.index=value.index;
  if(value.index>=0) {result.texCoord=uintNumber(value.texCoord,1);readTransform(result,objectMember(value.extensions,transformName));}
 })) return result;
 if(const auto* object=info(m,at)) {
  result.index=index(object);
  if(const auto* uv=member(*object,"texCoord")) {if(!uv->IsInt()) fail("texture texCoord must be integer");result.texCoord=uintNumber(uv->GetNumberAsDouble(),1);}
  if(member(*object,"channel")) fail("standard texture-info channels are fixed by their schema");
  if(const auto* extensions=objectMember(*object,"extensions")) readTransform(result,objectMember(*extensions,transformName));
 }return result;
}
bool empty(const GltfMaterialTextureMappingPatch& mapping) {return !mapping.texCoord && !mapping.channel && !mapping.offset && !mapping.scale && !mapping.rotation;}
template<class T> bool valueEqual(const T& a,const T& b) {
 if constexpr(std::is_same_v<T,double>) return std::bit_cast<uint64_t>(a)==std::bit_cast<uint64_t>(b);
 else if constexpr(std::is_same_v<T,std::array<double,2>>) return valueEqual(a[0],b[0]) && valueEqual(a[1],b[1]);
 else return a==b;
}
template<class Variant> bool leafEqual(const std::optional<Variant>& a,const std::optional<Variant>& b) {
 if(a.has_value()!=b.has_value()) return false;if(!a) return true;if(a->index()!=b->index()) return false;
 if(a->index()==0) return true;return valueEqual(std::get<1>(*a),std::get<1>(*b));
}
bool editsEqual(const GltfMaterialTextureEdit& a,const GltfMaterialTextureEdit& b) {
 return a.operation==b.operation && a.binding==b.binding && leafEqual(a.mapping.texCoord,b.mapping.texCoord) && leafEqual(a.mapping.channel,b.mapping.channel) &&
  leafEqual(a.mapping.offset,b.mapping.offset) && leafEqual(a.mapping.scale,b.mapping.scale) && leafEqual(a.mapping.rotation,b.mapping.rotation);
}
void validateMapping(const GltfMaterialTextureMappingPatch& mapping,const GltfMaterialTextureBinding&) {
 for(const auto* leaf:{&mapping.texCoord,&mapping.channel}) if(*leaf && (**leaf).index()==1)
  if(std::get<1>(**leaf)>(leaf==&mapping.texCoord?1u:4u)) fail("mapping integer exceeds its supported range");
 for(const auto* leaf:{&mapping.offset,&mapping.scale}) if(*leaf && (**leaf).index()==1) for(double number:std::get<1>(**leaf)) finite(number);
 if(mapping.rotation && mapping.rotation->index()==1) finite(std::get<1>(*mapping.rotation));
}
bool editObjectLeaf(O& object,std::string_view name,const auto& leaf) {
 if(!leaf) return false;if(leaf->index()==0) return remove(object,name);
 using T=std::decay_t<decltype(std::get<1>(*leaf))>;
 if constexpr(std::is_same_v<T,std::array<double,2>>) {V::Array values;for(double number:std::get<1>(*leaf)) values.emplace_back(number);return put(object,name,V(std::move(values)));}
 else if constexpr(std::is_same_v<T,uint32_t>) return put(object,name,V(int(std::get<1>(*leaf))));
 else return put(object,name,V(std::get<1>(*leaf)));
}
bool mappingExtensions(tinygltf::ExtensionMap& extensions,const GltfMaterialTextureMappingPatch& patch) {
 bool changed=false;auto found=extensions.find(key(transformName));
 if(found!=extensions.end() && !found->second.IsObject()) fail("texture transform must be an object");
 const auto sets=[](const auto& value) {return value && value->index()==1;};
 if(found==extensions.end() && !(sets(patch.offset)||sets(patch.scale)||sets(patch.rotation))) return false;
 if(found==extensions.end()) {found=extensions.emplace(key(transformName),V(O{})).first;changed=true;}
 auto& object=found->second.Get<O>();
 if(patch.texCoord) changed|=remove(object,"texCoord");
 changed|=editObjectLeaf(object,"offset",patch.offset);changed|=editObjectLeaf(object,"scale",patch.scale);changed|=editObjectLeaf(object,"rotation",patch.rotation);
 // Reset owns the selected leaf, not the existence of an already retained
 // transform object. Keeping an empty object also retains animation targets
 // whose values come from schema defaults.
 return changed;
}
void mappingLegacy(tinygltf::Parameter& parameter,const GltfMaterialTextureMappingPatch& patch) {
 auto& values=parameter.json_double_value;
 const auto edit=[&](const char* name,const auto& leaf) {if(!leaf) return;if(leaf->index()==0) values.erase(name);else values[name]=double(std::get<1>(*leaf));};
 edit("texCoord",patch.texCoord);edit("channel",patch.channel);edit("rotation",patch.rotation);
 const auto vector=[&](const char* x,const char* y,const auto& leaf) {if(!leaf) return;if(leaf->index()==0) {values.erase(x);values.erase(y);}else {values[x]=std::get<1>(*leaf)[0];values[y]=std::get<1>(*leaf)[1];}};
 vector("offsetX","offsetY",patch.offset);vector("scaleX","scaleY",patch.scale);
}
void clearAliases(tinygltf::Material& m,S slot,const Location* retain=nullptr) {
 for(const auto name:aliases(slot)) if(!name.empty()) for(bool additional:{false,true}) {
  if(retain && retain->legacy && retain->field==name && retain->additional==additional) continue;
  (additional?m.additionalValues:m.values).erase(key(name));
 }
}
void apply(tinygltf::Material& m,const GltfMaterialTextureEdit& edit,bool evaluated) {
 auto at=locate(m,edit.slot);const auto before=read(m,at);const bool clear=edit.operation==GltfMaterialTextureOperation::Clear;
 const bool bind=edit.operation==GltfMaterialTextureOperation::Bind;
 const int source=bind?(evaluated?edit.binding->evaluatedTexture:edit.binding->authoredTexture):-1;
 if(at.legacy) {
  if(clear) {clearAliases(m,edit.slot);return;}
  auto& map=at.additional?m.additionalValues:m.values;
  auto& parameter=map[key(at.field)];if(bind) {parameter.json_double_value["index"]=double(source);clearAliases(m,edit.slot,&at);}
  mappingLegacy(parameter,edit.mapping);return;
 }
 if(core(m,edit.slot,[&](auto& value) {
  if(clear) {
   value.index=-1;value.texCoord=0;
   GltfMaterialTextureMappingPatch reset;reset.offset=GltfMaterialTextureVectorValue{};reset.scale=GltfMaterialTextureVectorValue{};reset.rotation=GltfMaterialTextureScalarValue{};reset.texCoord=GltfMaterialTextureUIntValue{};
   if(mappingExtensions(value.extensions,reset)) value.extensions_json_string.clear();
  } else {
   if(bind) value.index=source;
   if(edit.mapping.texCoord) value.texCoord=edit.mapping.texCoord->index()==0?0:int(std::get<1>(*edit.mapping.texCoord));
   if(mappingExtensions(value.extensions,edit.mapping)) value.extensions_json_string.clear();
  }
 })) {
  if(bind||clear) {
   clearAliases(m,edit.slot);
   if(const auto name=coreAlias(edit.slot);!name.empty()) {
    m.values.erase(key(name));m.additionalValues.erase(key(name));
   }
  }
  return;
 }
 auto parent=m.extensions.find(key(at.extension));
 if(clear) {
  if(parent!=m.extensions.end() && remove(parent->second.Get<O>(),at.field)) m.extensions_json_string.clear();
  clearAliases(m,edit.slot);if(at.coupled && before.index>=0) clearAliases(m,*at.coupled);return;
 }
 if(parent==m.extensions.end()) parent=m.extensions.emplace(key(at.extension),V(O{})).first;
 auto& object=ensure(parent->second.Get<O>(),at.field);bool changed=bind?put(object,"index",V(source)):false;
 if(edit.mapping.texCoord) changed|=editObjectLeaf(object,"texCoord",edit.mapping.texCoord);
 const bool needsExtensions=edit.mapping.offset || edit.mapping.scale || edit.mapping.rotation ||
  (edit.mapping.texCoord && object.contains("extensions"));
 if(needsExtensions) {
  const bool existed=object.contains("extensions");auto& extensions=ensure(object,"extensions");
  changed|=mappingExtensions(extensions,edit.mapping);
  if(extensions.empty()) changed|=object.erase("extensions")!=0;
  else if(!existed) changed=true;
 }
 if(changed) m.extensions_json_string.clear();
 if(bind) {clearAliases(m,edit.slot);if(at.coupled) clearAliases(m,*at.coupled);}
 (void)before;
}
bool copyTransformLeaves(tinygltf::ExtensionMap& target,const tinygltf::ExtensionMap& source,uint8_t mask) {
 const auto* trusted=objectMember(source,transformName);auto current=target.find(key(transformName));
 bool needed=false;
 for(const auto [bit,name]:{std::pair{TextureTexCoord,"texCoord"},std::pair{TextureOffset,"offset"},std::pair{TextureScale,"scale"},std::pair{TextureRotation,"rotation"}})
  if((mask&bit) && trusted && member(*trusted,name)) needed=true;
 if(current==target.end() && !needed && !trusted) return false;
 bool changed=false;
 if(current==target.end()) {current=target.emplace(key(transformName),V(O{})).first;changed=true;}
 auto& object=current->second.Get<O>();
 for(const auto [bit,name]:{std::pair{TextureTexCoord,"texCoord"},std::pair{TextureOffset,"offset"},std::pair{TextureScale,"scale"},std::pair{TextureRotation,"rotation"}})
  if(mask&bit) changed|=copyMember(object,trusted,name);
 if(object.empty() && !trusted) changed|=target.erase(key(transformName))!=0;
 return changed;
}
void copyAliasChains(tinygltf::Material& target,const tinygltf::Material& source,S slot) {
 const auto copy=[&](std::string_view name) {
  if(name.empty()) return;
  for(bool additional:{false,true}) {
   auto& to=additional?target.additionalValues:target.values;const auto& from=additional?source.additionalValues:source.values;
   if(const auto i=from.find(key(name));i!=from.end()) to[key(name)]=i->second;else to.erase(key(name));
  }
 };
 for(const auto name:aliases(slot)) copy(name);
 // Specular-glossiness BaseColor owns diffuseTexture, not the retained
 // metallic-roughness baseColorTexture representation.
 if(core(source,slot,[](const auto&) {})) copy(coreAlias(slot));
}
} // namespace

std::span<const GltfMaterialTextureSlotInfo> GltfMaterialTextureSlotRegistry() noexcept {return slots;}
const GltfMaterialTextureSlotInfo& GltfMaterialTextureSlotDescription(S slot) {if(size_t(slot)>=slots.size()) fail("unknown texture slot");return slots[size_t(slot)];}
GltfMaterialTextureBinding ReadGltfMaterialTextureBinding(const tinygltf::Material& m,S slot) {return read(m,locate(m,slot));}
void ValidateGltfMaterialTextureSourceReferences(const tinygltf::Material& material,size_t textureCount) {
 for(const auto& descriptor:slots) {
  const auto binding=ReadGltfMaterialTextureBinding(material,descriptor.slot);
  if(binding.index>=0 && size_t(binding.index)>=textureCount) fail("selected texture references a missing source identity");
  // A lower-precedence fallback must also be valid; an existing standard
  // binding must not hide a forged legacy source or malformed mapping.
  const auto validateAlias=[&](std::string_view name) {
   if(name.empty()) return;
   for(bool additional:{false,true}) {
    const auto& map=additional?material.additionalValues:material.values;
    if(!map.contains(key(name))) continue;
    const auto fallback=read(material,{descriptor.slot,{},name,name,true,additional,{}});
    if(fallback.index<0 || size_t(fallback.index)>=textureCount) fail("legacy fallback references a missing source identity");
   }
  };
  for(const auto name:aliases(descriptor.slot)) validateAlias(name);
  validateAlias(coreAlias(descriptor.slot));
 }
}
uint8_t GltfMaterialTextureEditOwnership(const GltfMaterialTextureEdit& edit) noexcept {
 uint8_t result=edit.operation==GltfMaterialTextureOperation::Mapping?0:TextureBinding;
 if(edit.operation==GltfMaterialTextureOperation::Clear) return TextureBinding|TextureTexCoord|TextureOffset|TextureScale|TextureRotation|TextureChannel|TextureContainer;
 if(edit.mapping.texCoord) result|=TextureTexCoord;if(edit.mapping.offset) result|=TextureOffset;
 if(edit.mapping.scale) result|=TextureScale;if(edit.mapping.rotation) result|=TextureRotation;if(edit.mapping.channel) result|=TextureChannel;return result;
}
void ValidateGltfMaterialTextureEdits(std::span<const GltfMaterialTextureEdit> edits,const tinygltf::Material& authored,
    const tinygltf::Material& evaluated,size_t authoredTextures,size_t evaluatedTextures) {
 if(edits.size()>size_t(S::Count)) fail("texture edit count exceeds the slot registry");
 std::array<bool,size_t(S::Count)> seen{};
 for(size_t i=0;i<edits.size();++i) {
  const auto& edit=edits[i];GltfMaterialTextureSlotDescription(edit.slot);
  if(seen[size_t(edit.slot)]) fail("duplicate texture slot edit");seen[size_t(edit.slot)]=true;
  if(edit.operation!=GltfMaterialTextureOperation::Bind && edit.operation!=GltfMaterialTextureOperation::Clear && edit.operation!=GltfMaterialTextureOperation::Mapping) fail("unknown texture operation");
  if(edit.operation==GltfMaterialTextureOperation::Bind) {
   if(!edit.binding || edit.binding->authoredTexture<0 || edit.binding->evaluatedTexture<0 ||
      size_t(edit.binding->authoredTexture)>=authoredTextures || size_t(edit.binding->evaluatedTexture)>=evaluatedTextures) fail("binding requires explicit valid authored/evaluated source IDs");
  } else if(edit.binding) fail("only Bind may supply a source pair");
  if(edit.operation==GltfMaterialTextureOperation::Clear && !empty(edit.mapping)) fail("Clear cannot also edit mapping");
  if(edit.operation==GltfMaterialTextureOperation::Mapping && empty(edit.mapping)) fail("empty mapping edit");
  for(const auto* material:{&authored,&evaluated}) {
   const auto current=ReadGltfMaterialTextureBinding(*material,edit.slot);
   if(edit.operation==GltfMaterialTextureOperation::Mapping && current.index<0) fail("mapping edit requires an existing binding");
   validateMapping(edit.mapping,current);
   if(edit.mapping.channel && !current.channelEditable && edit.mapping.channel->index()==1 &&
       std::get<1>(*edit.mapping.channel)!=GltfMaterialTextureSlotDescription(edit.slot).channel) fail("standard texture channels are fixed by schema");
   const auto at=locate(*material,edit.slot);
   for(size_t j=0;j<i;++j) {
    const auto other=locate(*material,edits[j].slot);
    if(sameField(at,other) && !editsEqual(edit,edits[j])) fail("coupled texture source field has conflicting edits");
    if(isSg(*material) && ((edit.slot==S::Roughness && edits[j].slot==S::SpecularColor) ||
        (edit.slot==S::SpecularColor && edits[j].slot==S::Roughness)) && !sameField(at,other)) fail("ambiguous specular-glossiness fallback and shared-field edits");
   }
  }
 }
}
void ApplyGltfMaterialTextureEdits(tinygltf::Material& material,std::span<const GltfMaterialTextureEdit> edits,bool evaluated) {
 for(const auto& edit:edits) apply(material,edit,evaluated);
}
void CopyGltfMaterialTextureEditLeaves(tinygltf::Material& target,const tinygltf::Material& source,S slot,uint8_t mask) {
 const auto from=locate(source,slot),to=locate(target,slot);const auto trusted=read(source,from),current=read(target,to);
 if(mask&TextureBinding) {
  copyAliasChains(target,source,slot);if(from.coupled) copyAliasChains(target,source,*from.coupled);
 }
 if(from.legacy || to.legacy) {
  if(mask&TextureBinding) {
   const auto canonical=primary(source,slot);
   if(!canonical.legacy) {
    const auto* desired=info(source,canonical);auto parent=target.extensions.find(key(canonical.extension));
    if(desired) ensure(target.extensions,canonical.extension)[key(canonical.field)]=V(*desired);
    else if(parent!=target.extensions.end()) remove(parent->second.Get<O>(),canonical.field);
    if(exactMap(target.extensions,source.extensions)) target.extensions_json_string=source.extensions_json_string;
    else target.extensions_json_string.clear();
   }
   return;
  }
  if(!sameField(from,to) || current.index<0 || trusted.index<0) fail("current texture representation changed during mapping replay");
  auto& numbers=(to.additional?target.additionalValues:target.values).at(key(to.field)).json_double_value;
  const auto& original=parameter(source,from)->json_double_value;
  const auto copy=[&](const char* name) {if(const auto i=original.find(name);i!=original.end()) numbers[name]=i->second;else numbers.erase(name);};
  if(mask&TextureTexCoord) copy("texCoord");if(mask&TextureChannel) copy("channel");
  if(mask&TextureOffset) {copy("offsetX");copy("offsetY");}if(mask&TextureScale) {copy("scaleX");copy("scaleY");}if(mask&TextureRotation) copy("rotation");return;
 }
 if(core(target,slot,[&](auto& value) {
  core(source,slot,[&](const auto& original) {
   if constexpr(std::is_same_v<std::decay_t<decltype(value)>,std::decay_t<decltype(original)>>) {
    if((mask&TextureContainer) || ((mask&TextureBinding) && ((current.index<0)!=(trusted.index<0)))) {
     if constexpr(requires {value.scale;}) {const auto factor=value.scale;value=original;value.scale=factor;}
     else if constexpr(requires {value.strength;}) {const auto factor=value.strength;value=original;value.strength=factor;}
     else value=original;
     return;
    }
    if(mask&TextureBinding) value.index=original.index;
    if(mask&TextureTexCoord) value.texCoord=original.texCoord;
    const bool changed=copyTransformLeaves(value.extensions,original.extensions,mask);
    if(exactMap(value.extensions,original.extensions)) value.extensions_json_string=original.extensions_json_string;
    else if(changed) value.extensions_json_string.clear();
   }
  });
 })) return;
 const auto canonical=primary(source,slot);const auto* original=info(source,canonical);
 auto parent=target.extensions.find(key(canonical.extension));
 const bool hadParent=parent!=target.extensions.end();
 const auto* existing=hadParent?objectMember(parent->second.Get<O>(),canonical.field):nullptr;
 if((mask&TextureContainer) || ((mask&TextureBinding) && ((!existing)!=(!original)))) {
  if(original) ensure(target.extensions,canonical.extension)[key(canonical.field)]=V(*original);
  else if(hadParent) remove(parent->second.Get<O>(),canonical.field);
  target.extensions_json_string.clear();return;
 }
 if(!existing || !original) return;
 auto& object=parent->second.Get<O>().at(key(canonical.field)).Get<O>();bool changed=false;
 if(mask&TextureBinding) changed|=copyMember(object,original,"index");
 if(mask&TextureTexCoord) changed|=copyMember(object,original,"texCoord");
 const auto* originalExtensions=objectMember(*original,"extensions");
 const bool hasCurrentExtensions=object.contains("extensions");
 if(originalExtensions || hasCurrentExtensions) {
  auto& extensions=ensure(object,"extensions");const O emptyMap;
  changed|=copyTransformLeaves(extensions,originalExtensions?*originalExtensions:emptyMap,mask);
  if(extensions.empty() && !originalExtensions) changed|=object.erase("extensions")!=0;
 }
 if(changed) target.extensions_json_string.clear();
 if(exactMap(target.extensions,source.extensions)) target.extensions_json_string=source.extensions_json_string;
}
} // namespace container::geometry::gltf
