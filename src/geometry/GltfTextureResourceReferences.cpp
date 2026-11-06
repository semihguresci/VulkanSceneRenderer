#include <Container/geometry/GltfTextureResourceReferences.h>
#include <Container/geometry/GltfLegacyMaterialSerialization.h>
#include <Container/geometry/GltfMaterialTextureAuthoring.h>
#include <Container/geometry/GltfExtendedMaterials.h>

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace container::geometry::gltf {
namespace {
using Value=tinygltf::Value;
using Object=Value::Object;
using Kind=GltfTextureResourceReferenceKind;
using Incomplete=GltfTextureResourceIncompleteKind;
struct Range {size_t begin{},count{};};
struct Data {
  GltfTextureResourceReferenceLimits limits;
  GltfTextureResourceReferenceStats stats;
  std::vector<uint8_t> materialActive,textureActive,imageActive,samplerActive;
  std::vector<Range> materialRows,imageRows,samplerRows;
  std::vector<uint32_t> materials,images,samplers;
  std::vector<GltfTextureResourceReference> source;
  std::vector<GltfTextureResourceIncompleteReason> reasons;
  std::vector<std::byte> witness;
  size_t detachedTextures{},detachedImages{},detachedSamplers{};
};
enum class Domain { Other,Root,Material,Primitive,Node,Texture,TextureInfo,View,Accessor,Target,Light };
constexpr std::array<std::string_view,13> materialExtensions{
    "KHR_materials_unlit","KHR_materials_emissive_strength","KHR_materials_ior","KHR_materials_dispersion",
    "KHR_materials_transmission","KHR_materials_volume","KHR_materials_clearcoat","KHR_materials_sheen",
    "KHR_materials_specular","KHR_materials_iridescence","KHR_materials_anisotropy",
    "KHR_materials_diffuse_transmission","KHR_materials_pbrSpecularGlossiness"};
constexpr std::array<std::string_view,4> areaExtensions{
    "KHR_lights_area","EXT_lights_area","WEBGI_lights_area","WEBGI_area_light"};
constexpr std::array<std::string_view,4> imageExtensions{
    "KHR_texture_basisu","EXT_texture_webp","MSFT_texture_dds","EXT_texture_avif"};
constexpr std::array<std::string_view,5> coreTextureFields{
    "baseColorTexture","metallicRoughnessTexture","normalTexture","occlusionTexture","emissiveTexture"};
bool area(std::string_view name) {return std::ranges::find(areaExtensions,name)!=areaExtensions.end();}
bool allowed(std::string_view name,Domain domain) {
  if(name=="KHR_xmp_json_ld") return true;
  switch(domain) {
    case Domain::Root:return name=="KHR_materials_variants" || name=="KHR_lights_punctual" || area(name);
    case Domain::Material:return std::ranges::find(materialExtensions,name)!=materialExtensions.end() ||
        std::ranges::find(kGltfExperimentalMaterialExtensions,name)!=kGltfExperimentalMaterialExtensions.end() || name=="MSFT_lod";
    case Domain::Primitive:return name=="KHR_materials_variants" || name=="KHR_draco_mesh_compression";
    case Domain::Node:return name=="KHR_lights_punctual" || area(name) || name=="EXT_mesh_gpu_instancing" || name=="MSFT_lod" ||
        name=="KHR_node_visibility" || name=="KHR_node_selectability" || name=="KHR_node_hoverability";
    case Domain::Texture:return std::ranges::find(imageExtensions,name)!=imageExtensions.end();
    case Domain::TextureInfo:return name=="KHR_texture_transform";
    case Domain::View:return name=="EXT_meshopt_compression" || name=="KHR_meshopt_compression";
    case Domain::Accessor:return name=="KHR_accessor_float64";
    case Domain::Target:return name=="KHR_animation_pointer";
    case Domain::Light:return area(name);
    default:return false;
  }
}
bool validLimits(const GltfTextureResourceReferenceLimits& l) {
  for(const auto n:{l.maxTableEntries,l.maxObjects,l.maxReferences,l.maxMetadataValues,l.maxMetadataBytes,l.maxTraversalSteps,l.maxOwnedBytes})
    if(!n || n==UINT64_MAX || n>SIZE_MAX) return false;
  return l.maxTableEntries<=UINT32_MAX && l.maxMetadataDepth && l.maxMetadataDepth<=64 &&
      l.maxDeclarations && l.maxDeclarations<=4096 && l.maxIncompleteReasons && l.maxIncompleteReasons<=1'000'000 &&
      (l.policy==GltfTextureResourceReferencePolicy::RequireComplete || l.policy==GltfTextureResourceReferencePolicy::RecordIncomplete);
}
bool active(std::span<const uint8_t> mask,size_t index) {return mask.empty() || mask[index]!=0;}
bool validMask(std::span<const uint8_t> mask,size_t count) {
  return (mask.empty() || mask.size()==count) && std::ranges::all_of(mask,[](uint8_t n){return n<=1;});
}
// Keep this schema subset aligned with GltfAnimation.cpp::pointerTarget. A
// recognized owner does not make an arbitrary normative suffix non-referencing.
uint32_t materialPointerComponents(std::string_view property) noexcept {
  using Property=std::pair<std::string_view,uint32_t>;
  constexpr auto properties=std::to_array<Property>({
      {"pbrMetallicRoughness/baseColorFactor",4},{"pbrMetallicRoughness/metallicFactor",1},
      {"pbrMetallicRoughness/roughnessFactor",1},{"emissiveFactor",3},{"alphaCutoff",1},
      {"normalTexture/scale",1},{"occlusionTexture/strength",1},
      {"extensions/KHR_materials_emissive_strength/emissiveStrength",1},
      {"extensions/KHR_materials_ior/ior",1},{"extensions/KHR_materials_dispersion/dispersion",1},
      {"extensions/KHR_materials_transmission/transmissionFactor",1},
      {"extensions/KHR_materials_volume/thicknessFactor",1},{"extensions/KHR_materials_volume/attenuationDistance",1},
      {"extensions/KHR_materials_volume/attenuationColor",3},
      {"extensions/KHR_materials_clearcoat/clearcoatFactor",1},{"extensions/KHR_materials_clearcoat/clearcoatRoughnessFactor",1},
      {"extensions/KHR_materials_clearcoat/clearcoatNormalTexture/scale",1},
      {"extensions/KHR_materials_sheen/sheenColorFactor",3},{"extensions/KHR_materials_sheen/sheenRoughnessFactor",1},
      {"extensions/KHR_materials_specular/specularFactor",1},{"extensions/KHR_materials_specular/specularColorFactor",3},
      {"extensions/KHR_materials_iridescence/iridescenceFactor",1},{"extensions/KHR_materials_iridescence/iridescenceIor",1},
      {"extensions/KHR_materials_iridescence/iridescenceThicknessMinimum",1},
      {"extensions/KHR_materials_iridescence/iridescenceThicknessMaximum",1},
      {"extensions/KHR_materials_anisotropy/anisotropyStrength",1},{"extensions/KHR_materials_anisotropy/anisotropyRotation",1},
      {"extensions/KHR_materials_diffuse_transmission/diffuseTransmissionFactor",1},
      {"extensions/KHR_materials_diffuse_transmission/diffuseTransmissionColorFactor",3},
      {"extensions/KHR_materials_pbrSpecularGlossiness/diffuseFactor",4},
      {"extensions/KHR_materials_pbrSpecularGlossiness/specularFactor",3},
      {"extensions/KHR_materials_pbrSpecularGlossiness/glossinessFactor",1}});
  for(const auto& [name,components]:properties) if(property==name) return components;
  constexpr std::string_view transform="/extensions/KHR_texture_transform/";
  const auto at=property.find(transform);if(at==std::string_view::npos) return 0;
  const auto leaf=property.substr(at+transform.size());
  if(leaf!="offset" && leaf!="scale" && leaf!="rotation") return 0;
  constexpr auto textures=std::to_array<std::string_view>({
      "pbrMetallicRoughness/baseColorTexture","pbrMetallicRoughness/metallicRoughnessTexture",
      "normalTexture","occlusionTexture","emissiveTexture",
      "extensions/KHR_materials_specular/specularTexture","extensions/KHR_materials_specular/specularColorTexture",
      "extensions/KHR_materials_transmission/transmissionTexture","extensions/KHR_materials_clearcoat/clearcoatTexture",
      "extensions/KHR_materials_clearcoat/clearcoatRoughnessTexture","extensions/KHR_materials_clearcoat/clearcoatNormalTexture",
      "extensions/KHR_materials_volume/thicknessTexture","extensions/KHR_materials_sheen/sheenColorTexture",
      "extensions/KHR_materials_sheen/sheenRoughnessTexture","extensions/KHR_materials_iridescence/iridescenceTexture",
      "extensions/KHR_materials_iridescence/iridescenceThicknessTexture","extensions/KHR_materials_anisotropy/anisotropyTexture",
      "extensions/KHR_materials_diffuse_transmission/diffuseTransmissionTexture",
      "extensions/KHR_materials_diffuse_transmission/diffuseTransmissionColorTexture",
      "extensions/KHR_materials_pbrSpecularGlossiness/diffuseTexture",
      "extensions/KHR_materials_pbrSpecularGlossiness/specularGlossinessTexture"});
  return std::ranges::find(textures,property.substr(0,at))!=textures.end()?(leaf=="rotation"?1u:2u):0u;
}
bool pointerIndex(std::string_view value,uint64_t& index) noexcept {
  if(value.empty()) return false;
  const auto parsed=std::from_chars(value.data(),value.data()+value.size(),index);
  return (value.size()==1 || value.front()!='0') && parsed.ec==std::errc{} &&
      parsed.ptr==value.data()+value.size() && index<=INT32_MAX;
}
enum class Mode { Count,Capture,Compare };
struct Sink {
  Mode mode{};Data* output{};const Data* expected{};uint64_t cap{};
  uint64_t bytes{},materialRefs{},imageRefs{},samplerRefs{},sourceRefs{},reasonCount{},reasonBytes{};
  size_t owner{},reasonAt{};enum class Owner { Source,Material,Texture } ownerKind{Owner::Source};
  bool good{true};
  bool blob(const void* data,size_t count) {
    if(!good || bytes>cap || count>cap-bytes) return good=false;
    if(!count) return true;
    if(mode==Mode::Capture) {
      if(count>output->witness.capacity()-output->witness.size()) return good=false;
      const auto* first=static_cast<const std::byte*>(data);
      output->witness.insert(output->witness.end(),first,first+count);
    } else if(mode==Mode::Compare && (bytes>expected->witness.size() || count>expected->witness.size()-size_t(bytes) ||
        (count && std::memcmp(expected->witness.data()+size_t(bytes),data,count)!=0))) return good=false;
    bytes+=count;return true;
  }
  bool number(uint64_t value) {return blob(&value,sizeof(value));}
  bool text(std::string_view value) {return number(value.size()) && blob(value.data(),value.size());}
  void beginMaterial(size_t index) {
    ownerKind=Owner::Material;owner=index;
    if(mode==Mode::Capture) output->materialRows[index].begin=output->materials.size();
  }
  void endMaterial() {
    if(mode==Mode::Capture) {auto& r=output->materialRows[owner];r.count=output->materials.size()-r.begin;}
    ownerKind=Owner::Source;
  }
  void beginTexture(size_t index) {
    ownerKind=Owner::Texture;owner=index;
    if(mode==Mode::Capture) {output->imageRows[index].begin=output->images.size();output->samplerRows[index].begin=output->samplers.size();}
  }
  void endTexture() {
    if(mode==Mode::Capture) {
      auto& i=output->imageRows[owner];i.count=output->images.size()-i.begin;
      auto& s=output->samplerRows[owner];s.count=output->samplers.size()-s.begin;
    }
    ownerKind=Owner::Source;
  }
  bool reference(Kind kind,uint32_t index) {
    if(!number(uint64_t(kind)) || !number(index)) return false;
    if(ownerKind==Owner::Material) {++materialRefs;if(mode==Mode::Capture) output->materials.push_back(index);}
    else if(ownerKind==Owner::Texture && kind==Kind::Image) {++imageRefs;if(mode==Mode::Capture) output->images.push_back(index);}
    else if(ownerKind==Owner::Texture && kind==Kind::Sampler) {++samplerRefs;if(mode==Mode::Capture) output->samplers.push_back(index);}
    else {++sourceRefs;if(mode==Mode::Capture) output->source.push_back({kind,index});}
    return true;
  }
  bool incomplete(Incomplete kind,std::string_view schema) {
    ++reasonCount;reasonBytes+=schema.size();
    if(mode==Mode::Capture) output->reasons.push_back({kind,std::string(schema)});
    else if(mode==Mode::Compare) {
      if(reasonAt>=expected->reasons.size() || expected->reasons[reasonAt].kind!=kind || expected->reasons[reasonAt].schema!=schema) return good=false;
      ++reasonAt;
    }
    return number(uint64_t(kind)) && text(schema);
  }
};
struct Scan {
  const GltfTextureResourceReferenceLimits& limits;Sink& sink;
  const tinygltf::Model* model{};GltfTextureResourceMasks masks;size_t textures{},images{},samplers{};
  GltfTextureResourceReferenceStats stats;const char* error{};bool limited{};
  bool fail(const char* message,bool limit=false) {if(!error) {error=message;limited=limit;}return false;}
  bool charge(uint64_t& value,uint64_t n,uint64_t cap) {
    if(value>cap || n>cap-value) return fail("Texture reference traversal/storage limit exceeded",true);
    value+=n;return true;
  }
  bool work(uint64_t n=1) {return charge(stats.traversalSteps,n,limits.maxTraversalSteps);}
  bool bytes(uint64_t n) {return charge(stats.metadataBytes,n,limits.maxMetadataBytes);}
  bool token(uint64_t n) {return sink.number(n) || fail("Texture reference witness capacity exceeded",true);}
  bool text(std::string_view value) {return bytes(value.size()) && work() && (sink.text(value) || fail("Texture reference witness capacity exceeded",true));}
  bool table(size_t n) {return n<=limits.maxTableEntries || fail("Texture reference table exceeds its limit",true);}
  const Value* member(const Value& value,std::string_view name) {
    if(value.IsObject()) for(const auto& [key,body]:value.Get<Object>()) {
      if(!work()) return nullptr;if(key==name) return &body;
    }
    return nullptr;
  }
  const Value* extension(const tinygltf::ExtensionMap& values,std::string_view name) {
    for(const auto& [key,body]:values) {if(!work()) return nullptr;if(key==name) return &body;}return nullptr;
  }
  bool incomplete(Incomplete kind,std::string_view schema) {
    if(limits.policy==GltfTextureResourceReferencePolicy::RequireComplete) return fail("Unproved normative texture reference schema");
    if(sink.reasonCount>=limits.maxIncompleteReasons) return fail("Incomplete texture reference reason limit exceeded",true);
    return sink.incomplete(kind,schema) || fail("Texture reference witness capacity exceeded",true);
  }
  bool declared(std::string_view name) {
    if(!model) return true; // Detached endpoint has no declaration namespace.
    for(const auto& entry:model->extensionsUsed) {if(!work()) return false;if(entry==name) return true;}
    return fail("Normative extension is absent from extensionsUsed");
  }
  bool nested(const Value& value) {
    if(!value.IsObject()) return fail("Nested normative extensions must be an object");
    for(const auto& [name,body]:value.Get<Object>()) {
      if(!work()) return false;
      const bool known=name=="KHR_texture_transform" || area(name) || name=="KHR_xmp_json_ld";
      if(!known && !incomplete(Incomplete::UnknownExtension,name)) return false;
      if(known && (!body.IsObject() || !declared(name))) return fail("Malformed nested normative extension");
    }
    return true;
  }
  bool metadata(const Value& root,bool inspect=true) {
    struct Frame {const Value* value{};size_t next{};Object::const_iterator member;bool entered{},inspect{};};
    std::array<Frame,65> stack;size_t depth{};stack[0].value=&root;stack[0].inspect=inspect;
    for(;;) {
      auto& frame=stack[depth];const auto& value=*frame.value;
      if(!frame.entered) {
        if(!work() || !charge(stats.metadataValues,1,limits.maxMetadataValues) || !bytes(sizeof(Value)) || !token(uint64_t(value.Type()))) return false;
        if((!value.IsString() && !value.Get<std::string>().empty()) || (!value.IsBinary() && !value.Get<std::vector<unsigned char>>().empty()) ||
            (!value.IsArray() && !value.Get<Value::Array>().empty()) || (!value.IsObject() && !value.Get<Object>().empty()))
          return fail("Malformed inactive metadata storage");
        if(value.IsString()) {if(!text(value.Get<std::string>())) return false;}
        else if(value.IsBinary()) {
          const auto& b=value.Get<std::vector<unsigned char>>();if(!bytes(b.size()) || !token(b.size())) return false;
          if(!sink.blob(b.data(),b.size())) return fail("Texture reference witness capacity exceeded",true);
        }
        else if(value.IsNumber()) {
          const auto n=value.GetNumberAsDouble();if(!std::isfinite(n)) return fail("Nonfinite texture reference metadata");
          if(!token(std::bit_cast<uint64_t>(n))) return false;
        } else if(value.IsBool()) {if(!token(value.Get<bool>())) return false;}
        else if(!value.IsArray() && !value.IsObject() && value.Type()!=tinygltf::NULL_TYPE) return fail("Unknown metadata type");
        frame.entered=true;frame.next=0;
        if(value.IsObject()) {if(!token(value.Get<Object>().size())) return false;frame.member=value.Get<Object>().begin();}
        else if(value.IsArray() && !token(value.Get<Value::Array>().size())) return false;
      }
      const Value* child{};bool inspectChild=frame.inspect;
      if(value.IsArray() && frame.next<value.Get<Value::Array>().size()) child=&value.Get<Value::Array>()[frame.next++];
      else if(value.IsObject() && frame.member!=value.Get<Object>().end()) {
        const auto& [name,next]=*frame.member++;if(!text(name)) return false;
        if(!frame.inspect || name=="extras") inspectChild=false;
        else if(name=="extensions" && !nested(next)) return false;
        child=&next;
      }
      if(child) {
        if(depth==limits.maxMetadataDepth) return fail("Texture reference metadata depth exceeded",true);
        auto& next=stack[++depth];next.value=child;next.entered=false;next.inspect=inspectChild;
      } else if(!depth) return true;
      else --depth;
    }
  }
  bool extensions(const tinygltf::ExtensionMap& values,Domain domain) {
    if(!token(values.size())) return false;
    for(const auto& [name,body]:values) {
      if(!text(name)) return false;
      const bool known=allowed(name,domain);
      if(!known && !incomplete(Incomplete::UnknownExtension,name)) return false;
      if(known && (!body.IsObject() || !declared(name))) return fail("Malformed normative extension object");
      if(!metadata(body)) return false;
    }
    return true;
  }
  template<class T> bool property(const T& object,Domain domain=Domain::Other) {
    if(!charge(stats.objects,1,limits.maxObjects) || !work() || !token(0x100u+uint64_t(domain))) return false;
    if constexpr(requires{object.name;}) if(!text(object.name)) return false;
    if constexpr(requires{object.extras;}) if(!metadata(object.extras,false)) return false;
    if constexpr(requires{object.extensions;}) if(!extensions(object.extensions,domain)) return false;
    if constexpr(requires{object.extras_json_string;}) if(!text(object.extras_json_string)) return false;
    if constexpr(requires{object.extensions_json_string;}) if(!text(object.extensions_json_string)) return false;
    return true;
  }
  bool reference(Kind kind,int64_t index,bool optional=false) {
    if(!charge(stats.references,1,limits.maxReferences) || !work() || !token(uint64_t(index))) return false;
    if(optional && index== -1) return true;
    const auto count=kind==Kind::Texture?textures:kind==Kind::Image?images:samplers;
    const auto mask=kind==Kind::Texture?masks.textures:kind==Kind::Image?masks.images:masks.samplers;
    if(index<0 || uint64_t(index)>=count || !active(mask,size_t(index))) return fail("Source resource reference targets a missing or retired identity");
    return sink.reference(kind,uint32_t(index)) || fail("Texture reference witness differs or exceeds capacity");
  }
  bool info(const Value& value) {
    if(!value.IsObject()) return fail("Material textureInfo must be an object");
    const auto* index=member(value,"index");
    if(!index || !index->IsInt()) return fail("Material textureInfo requires an exact integer index");
    return reference(Kind::Texture,index->Get<int>());
  }
  bool legacy(const tinygltf::ParameterMap& values) {
    if(!table(values.size()) || !token(values.size())) return false;
    for(const auto& [name,value]:values) {
      if(!text(name) || !text(value.string_value) || !token(value.bool_value) || !token(value.has_number_value) ||
          !table(value.number_array.size()) || !table(value.json_double_value.size()) || !token(value.number_array.size()) ||
          !token(value.json_double_value.size())) return false;
      if(!std::isfinite(value.number_value) || !token(std::bit_cast<uint64_t>(value.number_value))) return fail("Invalid legacy material numeric value");
      for(const auto n:value.number_array) if(!work() || !bytes(sizeof(double)) || !std::isfinite(n) || !token(std::bit_cast<uint64_t>(n))) return fail("Invalid legacy material numeric array");
      const double* index{};
      for(const auto& [key,n]:value.json_double_value) {
        if(!text(key) || !std::isfinite(n) || !token(std::bit_cast<uint64_t>(n))) return fail("Invalid legacy material texture metadata");
        if(key=="index") index=&n;
      }
      const bool known=std::ranges::find(coreTextureFields,name)!=coreTextureFields.end() ||
          std::ranges::find(KnownLegacyMaterialTextureFields(),name)!=KnownLegacyMaterialTextureFields().end();
      if(known) {
        if(!index || *index<0 || *index>INT32_MAX || std::floor(*index)!=*index) return fail("Legacy texture index must be an exact nonnegative integer");
        if(!reference(Kind::Texture,int64_t(*index))) return false;
      } else {
        constexpr std::array<std::string_view,8> factors{"baseColorFactor","metallicFactor","roughnessFactor","emissiveFactor",
            "alphaMode","alphaCutoff","doubleSided","diffuseFactor"};
        const bool factor=std::ranges::find(factors,name)!=factors.end() ||
            std::ranges::find(KnownLegacyMaterialFactorFields(),name)!=KnownLegacyMaterialFactorFields().end();
        if((index || !factor) && !incomplete(Incomplete::UnknownExtension,name)) return false;
      }
    }
    return true;
  }
  bool material(const tinygltf::Material& value) {
    if(!property(value,Domain::Material) || !property(value.pbrMetallicRoughness) ||
        !property(value.pbrMetallicRoughness.baseColorTexture,Domain::TextureInfo) ||
        !property(value.pbrMetallicRoughness.metallicRoughnessTexture,Domain::TextureInfo) ||
        !property(value.normalTexture,Domain::TextureInfo) || !property(value.occlusionTexture,Domain::TextureInfo) ||
        !property(value.emissiveTexture,Domain::TextureInfo)) return false;
    for(const auto id:{value.pbrMetallicRoughness.baseColorTexture.index,value.pbrMetallicRoughness.metallicRoughnessTexture.index,
        value.normalTexture.index,value.occlusionTexture.index,value.emissiveTexture.index}) if(!reference(Kind::Texture,id,true)) return false;
    // Read registry fields directly; effective workflow selection would hide
    // retained lower-precedence core/extension/legacy references.
    for(const auto& slot:GltfMaterialTextureSlotRegistry()) if(!slot.extension.empty()) {
      if(!work()) return false;
      const auto* body=extension(value.extensions,slot.extension);if(!body) continue;
      const auto field=slot.path.substr(slot.path.rfind('/')+1);
      if(const auto* binding=member(*body,field);binding && !info(*binding)) return false;
    }
    for(const auto& [name,field]:kGltfExperimentalMaterialTextures)
      if(const auto* body=extension(value.extensions,name))
        if(const auto* binding=member(*body,field);binding && !info(*binding)) return false;
    if(const auto* sg=extension(value.extensions,"KHR_materials_pbrSpecularGlossiness"))
      for(const auto field:{"diffuseTexture","specularGlossinessTexture"}) if(const auto* binding=member(*sg,field);binding && !info(*binding)) return false;
    return !error && legacy(value.values) && legacy(value.additionalValues);
  }
  bool texture(const tinygltf::Texture& value) {
    if(!property(value,Domain::Texture) || !reference(Kind::Image,value.source,true) || !reference(Kind::Sampler,value.sampler,true)) return false;
    for(const auto name:imageExtensions) if(const auto* body=extension(value.extensions,name)) {
      const auto* index=member(*body,"source");if(!index || !index->IsInt()) return fail("Alternate texture source requires an exact image index");
      if(!reference(Kind::Image,index->Get<int>())) return false;
    }
    return !error;
  }
  bool pointer(const tinygltf::AnimationChannel& channel) {
    const auto* body=extension(channel.target_extensions,"KHR_animation_pointer");
    if(!body) return !error && (channel.target_path!="pointer" || fail("Pointer channel lacks KHR_animation_pointer"));
    const auto* field=member(*body,"pointer");
    if(channel.target_path!="pointer" || channel.target_node!= -1 || !field || !field->IsString()) return fail("Malformed raw animation pointer");
    const std::string_view path=field->Get<std::string>();
    return pointerPath(path);
  }
  bool pointerPath(std::string_view path) {
    if(path.empty() || path.front()!='/') return fail("Malformed raw animation pointer path");
    for(size_t i=0;i<path.size();++i) {
      if(!work()) return false;
      if(path[i]=='~' && (++i==path.size() || (path[i]!='0' && path[i]!='1'))) return fail("Malformed JSON pointer escape");
    }
    const auto indexMutation=[&] {
      for(size_t at=1;at<path.size();) {
        const auto end=path.find('/',at);const auto part=path.substr(at,end-at);
        if(part=="index" || part=="source" || part=="sampler") return true;
        if(end==std::string_view::npos) break;at=end+1;
      }
      return false;
    };
    for(const auto [root,kind]:{std::pair<std::string_view,Kind>{"/images/",Kind::Image},{"/textures/",Kind::Texture},{"/samplers/",Kind::Sampler}}) {
      if(!path.starts_with(root)) continue;
      const auto tail=path.substr(root.size());const auto end=tail.find('/');const auto number=tail.substr(0,end);uint64_t id{};
      if(!pointerIndex(number,id)) return fail("Malformed resource pointer index");
      if(!reference(kind,int64_t(id))) return false;
      return incomplete(indexMutation()?Incomplete::AnimatedResourceIndex:Incomplete::UnknownAnimationPointer,path);
    }
    const auto propertyMatches=[&](std::string_view property,uint32_t components,bool allowComponent=true) {
      if(!components) return incomplete(Incomplete::UnknownAnimationPointer,path);
      const auto slash=property.rfind('/');uint64_t element{};
      if(slash!=std::string_view::npos && pointerIndex(property.substr(slash+1),element) &&
          (!allowComponent || components==1 || element>=components)) return fail("Animation pointer component is outside its supported property");
      return true;
    };
    const auto splitComponent=[](std::string_view property) {
      const auto at=property.rfind('/');uint64_t element{};
      return at!=std::string_view::npos && pointerIndex(property.substr(at+1),element)?property.substr(0,at):property;
    };
    for(const auto root:{std::string_view("/materials/"),std::string_view("/nodes/"),std::string_view("/cameras/"),
        std::string_view("/extensions/KHR_lights_punctual/lights/")}) {
      if(!path.starts_with(root)) continue;
      const auto tail=path.substr(root.size());const auto end=tail.find('/');uint64_t id{};
      if(!pointerIndex(tail.substr(0,end),id)) return fail("Malformed animation pointer owner index");
      const auto property=end==std::string_view::npos?std::string_view{}:tail.substr(end+1);
      const auto base=splitComponent(property);uint32_t components{};bool allowComponent=true;
      if(root=="/materials/") {
        if(id>=model->materials.size() || !active(masks.materials,size_t(id))) return fail("Material pointer targets a missing or retired owner");
        if(indexMutation()) return incomplete(Incomplete::AnimatedResourceIndex,path);
        components=materialPointerComponents(base);
      } else if(root=="/nodes/") {
        if(id>=model->nodes.size()) return fail("Animation pointer targets a missing node");
        if(base=="translation" || base=="scale") components=3;
        else if(base=="rotation") {components=4;allowComponent=false;}
        else if(base=="weights") {
          const auto mesh=model->nodes[size_t(id)].mesh;
          if(mesh<0 || size_t(mesh)>=model->meshes.size() || model->meshes[size_t(mesh)].primitives.empty())
            return fail("Animation pointer weights target lacks its mesh");
          const auto count=model->meshes[size_t(mesh)].primitives[0].targets.size();
          if(!count || count>UINT32_MAX) return fail("Animation pointer weights target lacks morph targets");
          components=uint32_t(count);
        }
      } else if(root=="/cameras/") {
        if(id>=model->cameras.size()) return fail("Animation pointer targets a missing camera");
        const auto& camera=model->cameras[size_t(id)];
        if((camera.type=="perspective" && (base=="perspective/yfov" || base=="perspective/znear" ||
            base=="perspective/zfar" || base=="perspective/aspectRatio")) ||
            (camera.type=="orthographic" && (base=="orthographic/xmag" || base=="orthographic/ymag" ||
            base=="orthographic/znear" || base=="orthographic/zfar"))) components=1;
      } else {
        if(id>=model->lights.size()) return fail("Animation pointer targets a missing light");
        if(base=="color") components=3;
        else if(base=="intensity" || base=="range" ||
            (model->lights[size_t(id)].type=="spot" && (base=="spot/innerConeAngle" || base=="spot/outerConeAngle"))) components=1;
      }
      return propertyMatches(property,components,allowComponent);
    }
    return incomplete(Incomplete::UnknownAnimationPointer,path);
  }
  bool source(const tinygltf::Model& source) {
    model=&source;textures=source.textures.size();images=source.images.size();samplers=source.samplers.size();
    if(!work(masks.materials.size()) || !work(masks.textures.size()) || !work(masks.images.size()) || !work(masks.samplers.size())) return false;
    if(!validMask(masks.materials,source.materials.size()) || !validMask(masks.textures,textures) ||
        !validMask(masks.images,source.images.size()) || !validMask(masks.samplers,source.samplers.size())) return fail("Invalid source active mask");
    for(const auto n:{source.buffers.size(),source.bufferViews.size(),source.accessors.size(),source.images.size(),source.samplers.size(),textures,
        source.materials.size(),source.meshes.size(),source.nodes.size(),source.skins.size(),source.cameras.size(),source.lights.size(),source.animations.size(),
        source.scenes.size(),source.audioEmitters.size(),source.audioSources.size()}) if(!table(n) || !work(n) || !token(n)) return false;
    if(source.extensionsUsed.size()>limits.maxDeclarations || source.extensionsRequired.size()>limits.maxDeclarations) return fail("Declaration count exceeds texture reference limit",true);
    for(const auto* names:{&source.extensionsUsed,&source.extensionsRequired}) {
      if(!token(names->size())) return false;
      for(const auto& name:*names) {
        if(!text(name)) return false;
        // Declarations alone have no outgoing resource edge. Unknown bodies
        // on active owners are rejected/recorded at their actual location;
        // an unused declaration left by a retired owner is harmless.
        if(names==&source.extensionsRequired && !declared(name)) return false;
      }
    }
    if(!property(source,Domain::Root) || !property(source.asset)) return false;
    for(const auto& value:source.buffers) if(!property(value)) return false;
    for(const auto& value:source.bufferViews) if(!property(value,Domain::View)) return false;
    for(const auto& value:source.accessors) {
      if(!property(value,Domain::Accessor) || !token(value.sparse.isSparse)) return false;
      if(value.sparse.isSparse && (!property(value.sparse) || !property(value.sparse.indices) || !property(value.sparse.values))) return false;
    }
    for(size_t i=0;i<source.images.size();++i) if(!token(active(masks.images,i)) || (active(masks.images,i) && !property(source.images[i]))) return false;
    for(size_t i=0;i<source.samplers.size();++i) if(!token(active(masks.samplers,i)) || (active(masks.samplers,i) && !property(source.samplers[i]))) return false;
    for(size_t i=0;i<source.textures.size();++i) {
      sink.beginTexture(i);if(!token(active(masks.textures,i)) || (active(masks.textures,i) && !texture(source.textures[i]))) return false;sink.endTexture();
    }
    for(size_t i=0;i<source.materials.size();++i) {
      sink.beginMaterial(i);if(!token(active(masks.materials,i)) || (active(masks.materials,i) && !material(source.materials[i]))) return false;sink.endMaterial();
    }
    for(const auto& mesh:source.meshes) {
      if(!property(mesh) || !table(mesh.primitives.size()) || !token(mesh.primitives.size())) return false;
      for(const auto& primitive:mesh.primitives) if(!property(primitive,Domain::Primitive)) return false;
    }
    for(const auto& value:source.nodes) if(!property(value,Domain::Node)) return false;
    for(const auto& value:source.skins) if(!property(value)) return false;
    for(const auto& value:source.cameras) if(!property(value) || !property(value.perspective) || !property(value.orthographic)) return false;
    for(const auto& value:source.lights) if(!property(value,Domain::Light) || !property(value.spot)) return false;
    for(const auto& value:source.scenes) if(!property(value)) return false;
    for(const auto& value:source.audioEmitters) if(!property(value) || !property(value.positional)) return false;
    for(const auto& value:source.audioSources) if(!property(value)) return false;
    for(const auto& animation:source.animations) {
      if(!property(animation) || !table(animation.samplers.size()) || !table(animation.channels.size()) ||
          !token(animation.samplers.size()) || !token(animation.channels.size())) return false;
      for(const auto& sampler:animation.samplers) if(!property(sampler)) return false;
      for(const auto& channel:animation.channels) {
        if(!property(channel) || !text(channel.target_path) || !metadata(channel.target_extras,false) ||
            !text(channel.target_extras_json_string) || !text(channel.target_extensions_json_string) ||
            !extensions(channel.target_extensions,Domain::Target) || !pointer(channel)) return false;
      }
    }
    return !error && sink.good;
  }
};
[[noreturn]] void throwError(const Scan& scan) {
  const auto* reason=scan.error?scan.error:"Texture reference witness does not match";
  if(scan.limited) throw std::length_error(reason);
  throw std::invalid_argument(reason);
}
uint64_t add(uint64_t a,uint64_t b) {if(b>UINT64_MAX-a) throw std::length_error("Texture reference size overflow");return a+b;}
uint64_t multiply(uint64_t a,uint64_t b) {if(a && b>UINT64_MAX/a) throw std::length_error("Texture reference size overflow");return a*b;}
uint64_t normalizationWork(const Sink& sink) {
  uint64_t result{};
  for(const auto count:{sink.materialRefs,sink.imageRefs,sink.samplerRefs,sink.sourceRefs}) {
    uint64_t levels=3;for(auto n=count;n>1;n=(n+1)/2) ++levels;
    result=add(result,multiply(count,levels));
  }
  return result;
}
uint64_t storage(const Sink& sink,size_t m,size_t t,size_t i,size_t s) {
  auto n=add(sizeof(Data)+512,sink.bytes);
  n=add(n,add(add(m,t),add(i,s)));
  n=add(n,multiply(add(m,multiply(t,2)),sizeof(Range)));
  n=add(n,multiply(add(add(sink.materialRefs,sink.imageRefs),sink.samplerRefs),sizeof(uint32_t)));
  n=add(n,multiply(sink.sourceRefs,sizeof(GltfTextureResourceReference)));
  n=add(n,add(multiply(sink.reasonCount,sizeof(GltfTextureResourceIncompleteReason)+128),sink.reasonBytes));return n;
}
void fillMask(std::vector<uint8_t>& out,std::span<const uint8_t> in,size_t count) {
  out.assign(count,uint8_t(1));if(!in.empty()) std::copy(in.begin(),in.end(),out.begin());
}
void reserve(Data& out,const Sink& count,size_t m,size_t t,size_t i,size_t s,GltfTextureResourceMasks masks) {
  fillMask(out.materialActive,masks.materials,m);fillMask(out.textureActive,masks.textures,t);
  fillMask(out.imageActive,masks.images,i);fillMask(out.samplerActive,masks.samplers,s);
  out.materialRows.resize(m);out.imageRows.resize(t);out.samplerRows.resize(t);
  out.materials.reserve(size_t(count.materialRefs));out.images.reserve(size_t(count.imageRefs));out.samplers.reserve(size_t(count.samplerRefs));
  out.source.reserve(size_t(count.sourceRefs));out.reasons.reserve(size_t(count.reasonCount));out.witness.reserve(size_t(count.bytes));
}
void compact(std::vector<uint32_t>& values,std::vector<Range>& rows) {
  size_t next{};
  for(auto& row:rows) {
    auto first=values.begin()+row.begin,last=first+row.count;std::sort(first,last);last=std::unique(first,last);
    const auto count=size_t(last-first);for(size_t i=0;i<count;++i) values[next+i]=first[i];row={next,count};next+=count;
  }
  values.resize(next);
}
void finish(Data& data) {
  compact(data.materials,data.materialRows);compact(data.images,data.imageRows);compact(data.samplers,data.samplerRows);
  std::sort(data.source.begin(),data.source.end(),[](const auto& a,const auto& b){return a.kind==b.kind?a.index<b.index:a.kind<b.kind;});
  data.source.erase(std::unique(data.source.begin(),data.source.end()),data.source.end());
}
std::span<const uint32_t> row(const std::vector<uint32_t>& refs,const std::vector<Range>& rows,uint32_t index) {
  if(index>=rows.size()) return {};const auto r=rows[index];return std::span(refs).subspan(r.begin,r.count);
}
bool sameMask(std::span<const uint8_t> mask,const std::vector<uint8_t>& expected) {
  if(!validMask(mask,expected.size())) return false;
  for(size_t i=0;i<expected.size();++i) if(active(mask,i)!=bool(expected[i])) return false;return true;
}
} // namespace

struct GltfTextureResourceReferences::State {Data data;};
struct GltfMaterialRawTextureReferences::State {Data data;};
struct GltfTextureRawResourceReferences::State {Data data;};
struct GltfTextureResourceReferenceAccess {
  static GltfTextureResourceReferences model(Data&& data) {
    GltfTextureResourceReferences result;auto state=std::make_shared<GltfTextureResourceReferences::State>();state->data=std::move(data);result.state_=std::move(state);return result;
  }
  static GltfMaterialRawTextureReferences material(Data&& data) {
    GltfMaterialRawTextureReferences result;auto state=std::make_shared<GltfMaterialRawTextureReferences::State>();state->data=std::move(data);result.state_=std::move(state);return result;
  }
  static GltfTextureRawResourceReferences texture(Data&& data) {
    GltfTextureRawResourceReferences result;auto state=std::make_shared<GltfTextureRawResourceReferences::State>();state->data=std::move(data);result.state_=std::move(state);return result;
  }
  static const Data* data(const GltfTextureResourceReferences& value) {return value.state_?&value.state_->data:nullptr;}
  static const Data* data(const GltfMaterialRawTextureReferences& value) {return value.state_?&value.state_->data:nullptr;}
  static const Data* data(const GltfTextureRawResourceReferences& value) {return value.state_?&value.state_->data:nullptr;}
};
GltfTextureResourceReferences::operator bool() const noexcept {return bool(state_);}
bool GltfTextureResourceReferences::complete() const noexcept {return state_ && state_->data.reasons.empty();}
std::span<const GltfTextureResourceIncompleteReason> GltfTextureResourceReferences::incompleteReasons() const noexcept {return state_?std::span<const GltfTextureResourceIncompleteReason>(state_->data.reasons):std::span<const GltfTextureResourceIncompleteReason>{};}
size_t GltfTextureResourceReferences::materialCount() const noexcept {return state_?state_->data.materialActive.size():0;}
size_t GltfTextureResourceReferences::textureCount() const noexcept {return state_?state_->data.textureActive.size():0;}
size_t GltfTextureResourceReferences::imageCount() const noexcept {return state_?state_->data.imageActive.size():0;}
size_t GltfTextureResourceReferences::samplerCount() const noexcept {return state_?state_->data.samplerActive.size():0;}
bool GltfTextureResourceReferences::materialActive(uint32_t i) const noexcept {return state_ && i<materialCount() && state_->data.materialActive[i];}
bool GltfTextureResourceReferences::textureActive(uint32_t i) const noexcept {return state_ && i<textureCount() && state_->data.textureActive[i];}
bool GltfTextureResourceReferences::imageActive(uint32_t i) const noexcept {return state_ && i<imageCount() && state_->data.imageActive[i];}
bool GltfTextureResourceReferences::samplerActive(uint32_t i) const noexcept {return state_ && i<samplerCount() && state_->data.samplerActive[i];}
std::span<const uint32_t> GltfTextureResourceReferences::materialTextures(uint32_t i) const noexcept {return state_?row(state_->data.materials,state_->data.materialRows,i):std::span<const uint32_t>{};}
std::span<const uint32_t> GltfTextureResourceReferences::textureImages(uint32_t i) const noexcept {return state_?row(state_->data.images,state_->data.imageRows,i):std::span<const uint32_t>{};}
std::span<const uint32_t> GltfTextureResourceReferences::textureSamplers(uint32_t i) const noexcept {return state_?row(state_->data.samplers,state_->data.samplerRows,i):std::span<const uint32_t>{};}
std::span<const GltfTextureResourceReference> GltfTextureResourceReferences::sourceReferences() const noexcept {return state_?std::span<const GltfTextureResourceReference>(state_->data.source):std::span<const GltfTextureResourceReference>{};}
GltfTextureResourceReferenceStats GltfTextureResourceReferences::stats() const noexcept {return state_?state_->data.stats:GltfTextureResourceReferenceStats{};}
GltfMaterialRawTextureReferences::operator bool() const noexcept {return bool(state_);}
bool GltfMaterialRawTextureReferences::complete() const noexcept {return state_ && state_->data.reasons.empty();}
std::span<const GltfTextureResourceIncompleteReason> GltfMaterialRawTextureReferences::incompleteReasons() const noexcept {return state_?std::span<const GltfTextureResourceIncompleteReason>(state_->data.reasons):std::span<const GltfTextureResourceIncompleteReason>{};}
std::span<const uint32_t> GltfMaterialRawTextureReferences::textures() const noexcept {return state_?std::span<const uint32_t>(state_->data.materials):std::span<const uint32_t>{};}
GltfTextureResourceReferenceStats GltfMaterialRawTextureReferences::stats() const noexcept {return state_?state_->data.stats:GltfTextureResourceReferenceStats{};}

GltfTextureResourceReferences ExtractGltfTextureResourceReferences(const tinygltf::Model& model,GltfTextureResourceMasks masks,
    const GltfTextureResourceReferenceLimits& limits) {
  if(!validLimits(limits)) throw std::invalid_argument("Texture reference extraction requires finite capacities and a valid policy");
  Sink count{Mode::Count,nullptr,nullptr,limits.maxOwnedBytes};Scan preflight{limits,count,nullptr,masks};
  if(!preflight.source(model)) throwError(preflight);
  const auto normalization=normalizationWork(count);if(!preflight.work(normalization)) throwError(preflight);
  const auto owned=storage(count,model.materials.size(),model.textures.size(),model.images.size(),model.samplers.size());
  if(owned>limits.maxOwnedBytes) throw std::length_error("Texture reference owned-byte budget exceeded");
  Data data;data.limits=limits;reserve(data,count,model.materials.size(),model.textures.size(),model.images.size(),model.samplers.size(),masks);
  Sink capture{Mode::Capture,&data,nullptr,limits.maxOwnedBytes};Scan scan{limits,capture,nullptr,masks};
  if(!scan.source(model) || !scan.work(normalization) || capture.bytes!=count.bytes || capture.reasonCount!=count.reasonCount) throwError(scan);
  data.stats=scan.stats;data.stats.ownedBytes=owned;finish(data);return GltfTextureResourceReferenceAccess::model(std::move(data));
}
bool GltfTextureResourceReferencesMatch(const tinygltf::Model& model,GltfTextureResourceMasks masks,
    const GltfTextureResourceReferences& proof) noexcept {
  const auto* data=GltfTextureResourceReferenceAccess::data(proof);if(!data) return false;
  if(model.materials.size()!=data->materialActive.size() || model.textures.size()!=data->textureActive.size() ||
      model.images.size()!=data->imageActive.size() || model.samplers.size()!=data->samplerActive.size() ||
      !sameMask(masks.materials,data->materialActive) || !sameMask(masks.textures,data->textureActive) ||
      !sameMask(masks.images,data->imageActive) || !sameMask(masks.samplers,data->samplerActive)) return false;
  try {
    Sink compare{Mode::Compare,nullptr,data,data->limits.maxOwnedBytes};Scan scan{data->limits,compare,nullptr,masks};
    return scan.source(model) && compare.bytes==data->witness.size() && compare.reasonAt==data->reasons.size();
  } catch(...) {return false;}
}
GltfMaterialRawTextureReferences ExtractGltfMaterialRawTextureReferences(const tinygltf::Material& material,size_t textureCount,
    std::span<const uint8_t> activeTextures,const GltfTextureResourceReferenceLimits& limits) {
  if(!validLimits(limits) || textureCount>limits.maxTableEntries || activeTextures.size()>limits.maxTraversalSteps || !validMask(activeTextures,textureCount))
    throw std::invalid_argument("Raw material reference extraction requires finite capacities and exact active texture phases");
  GltfTextureResourceMasks masks;masks.textures=activeTextures;
  Sink count{Mode::Count,nullptr,nullptr,limits.maxOwnedBytes};count.beginMaterial(0);Scan preflight{limits,count,nullptr,masks,textureCount};
  if(!preflight.work(activeTextures.size())) throwError(preflight);
  if(!preflight.material(material)) throwError(preflight);count.endMaterial();
  const auto normalization=normalizationWork(count);if(!preflight.work(normalization)) throwError(preflight);
  const auto owned=storage(count,1,0,0,0);if(owned>limits.maxOwnedBytes) throw std::length_error("Raw material reference owned-byte budget exceeded");
  Data data;data.limits=limits;data.detachedTextures=textureCount;reserve(data,count,1,0,0,0,{});
  Sink capture{Mode::Capture,&data,nullptr,limits.maxOwnedBytes};capture.beginMaterial(0);Scan scan{limits,capture,nullptr,masks,textureCount};
  if(!scan.work(activeTextures.size()) || !scan.material(material) || !scan.work(normalization)) throwError(scan);capture.endMaterial();data.stats=scan.stats;data.stats.ownedBytes=owned;
  finish(data);return GltfTextureResourceReferenceAccess::material(std::move(data));
}
bool GltfMaterialRawTextureReferencesMatch(const tinygltf::Material& material,size_t textureCount,std::span<const uint8_t> activeTextures,
    const GltfMaterialRawTextureReferences& proof) noexcept {
  const auto* data=GltfTextureResourceReferenceAccess::data(proof);
  if(!data || textureCount!=data->detachedTextures || activeTextures.size()>data->limits.maxTraversalSteps || !validMask(activeTextures,textureCount)) return false;
  try {
    GltfTextureResourceMasks masks;masks.textures=activeTextures;
    Sink compare{Mode::Compare,nullptr,data,data->limits.maxOwnedBytes};compare.beginMaterial(0);
    Scan scan{data->limits,compare,nullptr,masks,textureCount};
    return scan.work(activeTextures.size()) && scan.material(material) && compare.bytes==data->witness.size() && compare.reasonAt==data->reasons.size();
  } catch(...) {return false;}
}
GltfTextureRawResourceReferences::operator bool() const noexcept {return bool(state_);}
bool GltfTextureRawResourceReferences::complete() const noexcept {return state_ && state_->data.reasons.empty();}
std::span<const GltfTextureResourceIncompleteReason> GltfTextureRawResourceReferences::incompleteReasons() const noexcept {return state_?std::span<const GltfTextureResourceIncompleteReason>(state_->data.reasons):std::span<const GltfTextureResourceIncompleteReason>{};}
std::span<const uint32_t> GltfTextureRawResourceReferences::images() const noexcept {return state_?std::span<const uint32_t>(state_->data.images):std::span<const uint32_t>{};}
std::span<const uint32_t> GltfTextureRawResourceReferences::samplers() const noexcept {return state_?std::span<const uint32_t>(state_->data.samplers):std::span<const uint32_t>{};}
GltfTextureResourceReferenceStats GltfTextureRawResourceReferences::stats() const noexcept {return state_?state_->data.stats:GltfTextureResourceReferenceStats{};}
GltfTextureRawResourceReferences ExtractGltfTextureRawResourceReferences(const tinygltf::Texture& texture,size_t imageCount,size_t samplerCount,
    std::span<const uint8_t> imageActive,std::span<const uint8_t> samplerActive,const GltfTextureResourceReferenceLimits& limits) {
  if(!validLimits(limits) || imageCount>limits.maxTableEntries || samplerCount>limits.maxTableEntries ||
      imageActive.size()>limits.maxTraversalSteps || samplerActive.size()>limits.maxTraversalSteps ||
      !validMask(imageActive,imageCount) || !validMask(samplerActive,samplerCount))
    throw std::invalid_argument("Raw texture reference extraction requires finite capacities and exact active phases");
  GltfTextureResourceMasks masks;masks.images=imageActive;masks.samplers=samplerActive;
  Sink count{Mode::Count,nullptr,nullptr,limits.maxOwnedBytes};count.beginTexture(0);Scan preflight{limits,count,nullptr,masks,0,imageCount,samplerCount};
  if(!preflight.work(imageActive.size()) || !preflight.work(samplerActive.size())) throwError(preflight);
  if(!preflight.texture(texture)) throwError(preflight);count.endTexture();
  const auto normalization=normalizationWork(count);if(!preflight.work(normalization)) throwError(preflight);
  const auto owned=storage(count,0,1,0,0);if(owned>limits.maxOwnedBytes) throw std::length_error("Raw texture reference owned-byte budget exceeded");
  Data data;data.limits=limits;data.detachedImages=imageCount;data.detachedSamplers=samplerCount;reserve(data,count,0,1,0,0,{});
  Sink capture{Mode::Capture,&data,nullptr,limits.maxOwnedBytes};capture.beginTexture(0);Scan scan{limits,capture,nullptr,masks,0,imageCount,samplerCount};
  if(!scan.work(imageActive.size()) || !scan.work(samplerActive.size()) || !scan.texture(texture) || !scan.work(normalization)) throwError(scan);capture.endTexture();data.stats=scan.stats;data.stats.ownedBytes=owned;
  finish(data);return GltfTextureResourceReferenceAccess::texture(std::move(data));
}
bool GltfTextureRawResourceReferencesMatch(const tinygltf::Texture& texture,size_t imageCount,size_t samplerCount,
    std::span<const uint8_t> imageActive,std::span<const uint8_t> samplerActive,const GltfTextureRawResourceReferences& proof) noexcept {
  const auto* data=GltfTextureResourceReferenceAccess::data(proof);
  if(!data || imageCount!=data->detachedImages || samplerCount!=data->detachedSamplers ||
      imageActive.size()>data->limits.maxTraversalSteps || samplerActive.size()>data->limits.maxTraversalSteps ||
      !validMask(imageActive,imageCount) || !validMask(samplerActive,samplerCount)) return false;
  try {
    GltfTextureResourceMasks masks;masks.images=imageActive;masks.samplers=samplerActive;
    Sink compare{Mode::Compare,nullptr,data,data->limits.maxOwnedBytes};compare.beginTexture(0);
    Scan scan{data->limits,compare,nullptr,masks,0,imageCount,samplerCount};
    return scan.work(imageActive.size()) && scan.work(samplerActive.size()) && scan.texture(texture) &&
        compare.bytes==data->witness.size() && compare.reasonAt==data->reasons.size();
  } catch(...) {return false;}
}
bool IsSupportedGltfNonResourceAnimationPointer(const tinygltf::Model& model,std::string_view path) noexcept {
  GltfTextureResourceReferenceLimits limits;limits.policy=GltfTextureResourceReferencePolicy::RecordIncomplete;
  if(path.size()>limits.maxMetadataBytes || model.materials.size()>limits.maxTableEntries || model.nodes.size()>limits.maxTableEntries ||
      model.cameras.size()>limits.maxTableEntries || model.lights.size()>limits.maxTableEntries) return false;
  try {
    Sink count{Mode::Count,nullptr,nullptr,limits.maxOwnedBytes};
    Scan scan{limits,count,&model,{},model.textures.size(),model.images.size(),model.samplers.size()};
    return scan.pointerPath(path) && !count.reasonCount && !count.sourceRefs;
  } catch(...) {return false;}
}
} // namespace container::geometry::gltf
