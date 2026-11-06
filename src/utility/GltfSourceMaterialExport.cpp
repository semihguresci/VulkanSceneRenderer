#include <Container/utility/GltfSourceMaterialExport.h>

#include <Container/geometry/GltfLegacyMaterialSerialization.h>
#include <Container/utility/GltfRuntimeMaterialAuthoring.h>

#include <algorithm>
#include <array>
#include <climits>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace container::material {
namespace {
using Value=tinygltf::Value;using Object=Value::Object;
constexpr std::string_view sg="KHR_materials_pbrSpecularGlossiness";
[[noreturn]] void fail(std::string_view message) {throw std::runtime_error("Source-backed material export: "+std::string(message));}
uint64_t add(uint64_t a,uint64_t b) {if(b>UINT64_MAX-a) fail("byte reservation overflow");return a+b;}
uint64_t mul(uint64_t a,uint64_t b) {if(a && b>UINT64_MAX/a) fail("byte reservation overflow");return a*b;}
bool listed(std::string_view name,std::initializer_list<std::string_view> known) {
 return std::ranges::find(known,name)!=known.end();
}
constexpr std::array<std::string_view,5> coreTextureParameters{
 "baseColorTexture","metallicRoughnessTexture","normalTexture","occlusionTexture","emissiveTexture"};
bool textureParameter(std::string_view name) {
 return std::ranges::find(coreTextureParameters,name)!=coreTextureParameters.end() ||
     std::ranges::find(geometry::gltf::KnownLegacyMaterialTextureFields(),name)!=geometry::gltf::KnownLegacyMaterialTextureFields().end();
}
bool textureField(std::string_view extension,std::string_view name) {
 if(extension==sg) return name=="diffuseTexture" || name=="specularGlossinessTexture";
 for(const auto& slot:geometry::gltf::GltfMaterialTextureSlotRegistry())
  if(slot.extension==extension && slot.path.substr(slot.path.rfind('/')+1)==name) return true;
 return false;
}
bool numericField(std::string_view extension,std::string_view name) {
 for(const auto& property:geometry::gltf::GltfMaterialPropertyRegistry()) if(property.extension &&
     geometry::gltf::GltfMaterialExtensionName(*property.extension)==extension) {
  const auto prefix=std::string_view("extensions/");const auto path=property.path;
  const auto start=prefix.size()+extension.size()+1;
  if(path.starts_with(prefix) && path.size()>start && path.substr(start).find('/')==std::string_view::npos && path.substr(start)==name) return true;
 }
 return false;
}
void textureExtensions(const tinygltf::ExtensionMap& extensions) {
 for(const auto& [name,body]:extensions) {
  if(name!="KHR_texture_transform" || !body.IsObject()) fail("unknown normative texture extension needs an explicit source-ID remapper");
  for(const auto& [field,value]:body.Get<Object>()) {
   if(field=="extras") continue;
   if(field=="extensions" && value.IsObject() && value.Get<Object>().empty()) continue;
   if(!listed(field,{"offset","scale","rotation","texCoord"})) fail("unknown normative transform field needs an explicit source-ID remapper");
  }
 }
}
void textureInfo(const Value& value) {
 if(!value.IsObject()) fail("known texture info is not an object");
 for(const auto& [name,body]:value.Get<Object>()) {
  if(name=="extras") continue;
  if(name=="extensions") {if(!body.IsObject()) fail("texture extension map is invalid");textureExtensions(body.Get<Object>());continue;}
  if(!listed(name,{"index","texCoord","scale"})) fail("unknown normative texture-info field needs an explicit source-ID remapper");
 }
}
void requireRemappableMetadata(const tinygltf::Material& material) {
 // Extras are opaque annotations and are deliberately never traversed here.
 textureExtensions(material.pbrMetallicRoughness.baseColorTexture.extensions);
 textureExtensions(material.pbrMetallicRoughness.metallicRoughnessTexture.extensions);
 textureExtensions(material.normalTexture.extensions);textureExtensions(material.occlusionTexture.extensions);textureExtensions(material.emissiveTexture.extensions);
 if(!material.pbrMetallicRoughness.extensions.empty()) fail("unknown normative PBR extension needs an explicit source-ID remapper");
 for(const auto& [name,body]:material.extensions) {
  bool known=false;for(size_t e=0;e<size_t(geometry::gltf::GltfMaterialExtension::Count);++e)
   known|=geometry::gltf::GltfMaterialExtensionName(geometry::gltf::GltfMaterialExtension(e))==name;
  if(!known || !body.IsObject()) fail("unknown normative material extension needs an explicit source-ID remapper");
  for(const auto& [field,value]:body.Get<Object>()) {
   if(field=="extras") continue;
   if(field=="extensions" && value.IsObject() && value.Get<Object>().empty()) continue;
   if(textureField(name,field)) textureInfo(value);
   else if(!numericField(name,field)) fail("unknown normative material field needs an explicit source-ID remapper");
  }
 }
 for(const auto* map:{&material.values,&material.additionalValues}) for(const auto& [field,parameter]:*map) {
  if(textureParameter(field)) {
   for(const auto& [name,_]:parameter.json_double_value)
    if(!listed(name,{"index","texCoord","channel","offsetX","offsetY","scaleX","scaleY","rotation","strength","scale"}))
     fail("unknown normative legacy texture field needs an explicit source-ID remapper");
  } else if(!listed(field,{"name","baseColorFactor","metallicFactor","roughnessFactor","emissiveFactor","alphaMode","alphaCutoff","doubleSided"}) &&
      std::ranges::find(geometry::gltf::KnownLegacyMaterialFactorFields(),std::string_view(field))==geometry::gltf::KnownLegacyMaterialFactorFields().end())
   fail("unknown normative legacy parameter needs an explicit source-ID remapper");
 }
}
void remap(tinygltf::Material& material,std::span<const int32_t> inverse) {
 const auto source=[&](int index) {
  if(index==-1) return -1;
  if(index<0 || size_t(index)>=inverse.size() || inverse[size_t(index)]<0) fail("texture reference has no unique archive source identity");
  return inverse[size_t(index)];
 };
 material.pbrMetallicRoughness.baseColorTexture.index=source(material.pbrMetallicRoughness.baseColorTexture.index);
 material.pbrMetallicRoughness.metallicRoughnessTexture.index=source(material.pbrMetallicRoughness.metallicRoughnessTexture.index);
 material.normalTexture.index=source(material.normalTexture.index);material.occlusionTexture.index=source(material.occlusionTexture.index);
 material.emissiveTexture.index=source(material.emissiveTexture.index);
 for(auto& [name,body]:material.extensions) {
  if(!body.IsObject()) continue;bool changed=false;
  for(auto& [field,value]:body.Get<Object>()) if(textureField(name,field) && value.IsObject()) {
   auto& info=value.Get<Object>();const auto index=info.find("index");
   if(index==info.end() || !index->second.IsInt()) fail("known texture reference has no integer source identity");
   const int before=index->second.Get<int>(),after=source(before);
   if(before!=after) {index->second=Value(after);changed=true;}
  }
  if(changed) material.extensions_json_string.clear();
 }
 for(auto* map:{&material.values,&material.additionalValues}) for(auto& [field,parameter]:*map) if(textureParameter(field)) {
   const auto index=parameter.json_double_value.find("index");
   if(index==parameter.json_double_value.end() || !std::isfinite(index->second) || index->second<0 || index->second>INT_MAX || std::trunc(index->second)!=index->second)
    fail("legacy texture reference has no exact source identity");
   const int before=int(index->second),after=source(before);
   if(before!=after) index->second=double(after);
 }
}
} // namespace

tinygltf::Material ExportSourceBackedGltfMaterial(const Material& runtime,const tinygltf::Material& authored,
    const tinygltf::Material& evaluated,GltfMaterialConversionContext archive,GltfMaterialConversionContext live,
    std::span<const int32_t> mapping,const GltfSourceMaterialExportLimits& limits) {
 if(!limits.maxOwnedBytes || limits.maxOwnedBytes==UINT64_MAX || !limits.conversion.maxSourceTextures || limits.conversion.maxSourceTextures==UINT64_MAX ||
     !limits.conversion.maxRuntimeResources || limits.conversion.maxRuntimeResources==UINT64_MAX || mapping.size()!=archive.textureCount ||
     archive.textureCount>limits.conversion.maxSourceTextures || live.textureCount>limits.conversion.maxSourceTextures ||
     archive.textureCount>INT_MAX || live.textureCount>INT_MAX || archive.runtimeResourceCount!=live.runtimeResourceCount ||
     live.runtimeResourceCount>limits.conversion.maxRuntimeResources || live.runtimeResourceCount>UINT32_MAX ||
     archive.colorResources.size()!=archive.textureCount || archive.dataResources.size()!=archive.textureCount ||
     live.colorResources.size()!=live.textureCount || live.dataResources.size()!=live.textureCount)
  fail("source map shape or finite export capacity is invalid");
 if(!runtime.specularGlossinessWorkflow || !authored.extensions.contains(std::string(sg)) || !evaluated.extensions.contains(std::string(sg)))
  fail("an exact retained SG source is required");
 if(!authored.lods.empty() || !evaluated.lods.empty()) fail("material LOD export requires an explicit material mapping");
 const auto aBytes=geometry::gltf::GltfMaterialRecordStorageBytes(authored,limits.conversion.validation);
 const auto eBytes=geometry::gltf::GltfMaterialRecordStorageBytes(evaluated,limits.conversion.validation);
 const auto reservation=add(add(mul(add(aBytes,eBytes),12),mul(live.textureCount,sizeof(int32_t))),4*1024);
 if(reservation>limits.maxOwnedBytes) fail("selected-record conversion/remap exceeds its finite owned-byte budget");
 const auto current=ConvertGltfMaterial(evaluated,live,GltfMaterialConversionPolicy::StrictGltf,limits.conversion);
 if(!RuntimeMaterialsEqual(current.value,runtime)) fail("evaluated source no longer exactly reproduces the runtime material");
 geometry::gltf::ValidateGltfMaterialRecord(authored,{1,archive.textureCount,archive.extensionsUsed,{}},limits.conversion.validation);
 std::vector<int32_t> inverse(live.textureCount,-1);
 for(size_t a=0;a<mapping.size();++a) {
  const int e=mapping[a];if(e==-1) continue;
  if(e<0 || size_t(e)>=inverse.size()) fail("archive source mapping references a missing evaluated texture");
  if(inverse[size_t(e)]!=-1) fail("evaluated texture has ambiguous archive source identities");
  inverse[size_t(e)]=int32_t(a);
 }
 bool identity=true;
 for(size_t e=0;e<inverse.size();++e) {if(inverse[e]<0) fail("evaluated texture has no archive source identity");identity&=inverse[e]==int32_t(e);}
 if(!identity) requireRemappableMetadata(evaluated);
 auto result=evaluated;remap(result,inverse);
 const auto converted=ConvertGltfMaterial(result,archive,GltfMaterialConversionPolicy::StrictGltf,limits.conversion);
 if(!RuntimeMaterialsEqual(converted.value,runtime)) fail("archive source roles do not exactly reproduce the runtime material");
 if(ReadGltfMaterialSourceTextures(authored,archive.textureCount,limits.conversion).indices!=converted.sourceTextures)
  fail("authored and evaluated material bindings do not share the proved source identity pairs");
 return result;
}
} // namespace container::material
