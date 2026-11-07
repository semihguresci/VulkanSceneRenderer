#include <Container/utility/GltfRuntimeMaterialAuthoring.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <climits>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace container::material {
namespace {
using Value=tinygltf::Value;
using Object=Value::Object;
using Slot=GltfMaterialTextureSlot;
constexpr size_t count=size_t(Slot::Count);
constexpr uint32_t absent=UINT32_MAX;
constexpr std::array<const char*,count> names{"baseColorTexture","normalTexture","occlusionTexture","emissiveTexture",
    "metallicRoughnessTexture","roughnessTexture","metalnessTexture","specularTexture","heightTexture","opacityTexture",
    "transmissionTexture","specularColorTexture","clearcoatTexture","clearcoatRoughnessTexture","clearcoatNormalTexture",
    "thicknessTexture","sheenColorTexture","sheenRoughnessTexture","iridescenceTexture","iridescenceThicknessTexture",
    "anisotropyTexture","diffuseTransmissionTexture","diffuseTransmissionColorTexture"};
constexpr std::array<uint32_t Material::*,count> resources{
    &Material::baseColorTextureIndex,&Material::normalTextureIndex,&Material::occlusionTextureIndex,&Material::emissiveTextureIndex,
    &Material::metallicRoughnessTextureIndex,&Material::roughnessTextureIndex,&Material::metalnessTextureIndex,&Material::specularTextureIndex,
    &Material::heightTextureIndex,&Material::opacityTextureIndex,&Material::transmissionTextureIndex,&Material::specularColorTextureIndex,
    &Material::clearcoatTextureIndex,&Material::clearcoatRoughnessTextureIndex,&Material::clearcoatNormalTextureIndex,&Material::thicknessTextureIndex,
    &Material::sheenColorTextureIndex,&Material::sheenRoughnessTextureIndex,&Material::iridescenceTextureIndex,&Material::iridescenceThicknessTextureIndex,
    &Material::anisotropyTextureIndex,&Material::diffuseTransmissionTextureIndex,&Material::diffuseTransmissionColorTextureIndex};
constexpr std::array<TextureTransform Material::*,count> transforms{
    &Material::baseColorTextureTransform,&Material::normalTextureTransform,&Material::occlusionTextureTransform,&Material::emissiveTextureTransform,
    &Material::metallicRoughnessTextureTransform,&Material::roughnessTextureTransform,&Material::metalnessTextureTransform,&Material::specularTextureTransform,
    &Material::heightTextureTransform,&Material::opacityTextureTransform,&Material::transmissionTextureTransform,&Material::specularColorTextureTransform,
    &Material::clearcoatTextureTransform,&Material::clearcoatRoughnessTextureTransform,&Material::clearcoatNormalTextureTransform,&Material::thicknessTextureTransform,
    &Material::sheenColorTextureTransform,&Material::sheenRoughnessTextureTransform,&Material::iridescenceTextureTransform,&Material::iridescenceThicknessTextureTransform,
    &Material::anisotropyTextureTransform,&Material::diffuseTransmissionTextureTransform,&Material::diffuseTransmissionColorTextureTransform};
struct Scalar {const char* name;float Material::* member;};
constexpr std::array scalars{
    Scalar{"metallicFactor",&Material::metallicFactor},Scalar{"roughnessFactor",&Material::roughnessFactor},
    Scalar{"alphaCutoff",&Material::alphaCutoff},Scalar{"normalTextureScale",&Material::normalTextureScale},
    Scalar{"occlusionStrength",&Material::occlusionStrength},Scalar{"opacityFactor",&Material::opacityFactor},
    Scalar{"specularFactor",&Material::specularFactor},Scalar{"heightScale",&Material::heightScale},Scalar{"heightOffset",&Material::heightOffset},
    Scalar{"transmissionFactor",&Material::transmissionFactor},Scalar{"emissiveStrength",&Material::emissiveStrength},Scalar{"ior",&Material::ior},
    Scalar{"dispersion",&Material::dispersion},Scalar{"clearcoatFactor",&Material::clearcoatFactor},Scalar{"clearcoatRoughnessFactor",&Material::clearcoatRoughnessFactor},
    Scalar{"clearcoatNormalTextureScale",&Material::clearcoatNormalTextureScale},Scalar{"thicknessFactor",&Material::thicknessFactor},
    Scalar{"attenuationDistance",&Material::attenuationDistance},Scalar{"sheenRoughnessFactor",&Material::sheenRoughnessFactor},
    Scalar{"iridescenceFactor",&Material::iridescenceFactor},Scalar{"iridescenceIor",&Material::iridescenceIor},
    Scalar{"iridescenceThicknessMinimum",&Material::iridescenceThicknessMinimum},Scalar{"iridescenceThicknessMaximum",&Material::iridescenceThicknessMaximum},
    Scalar{"anisotropyStrength",&Material::anisotropyStrength},Scalar{"anisotropyRotation",&Material::anisotropyRotation},
    Scalar{"diffuseTransmissionFactor",&Material::diffuseTransmissionFactor}};
struct Vector {const char* name;glm::vec3 Material::* member;};
constexpr std::array vectors{Vector{"emissiveColor",&Material::emissiveColor},Vector{"specularColorFactor",&Material::specularColorFactor},
    Vector{"attenuationColor",&Material::attenuationColor},Vector{"sheenColorFactor",&Material::sheenColorFactor},
    Vector{"diffuseTransmissionColorFactor",&Material::diffuseTransmissionColorFactor},
    Vector{"specularGlossinessDiffuse",&Material::specularGlossinessDiffuse},
    Vector{"specularGlossinessSpecular",&Material::specularGlossinessSpecular}};
struct ExtensionSlot {Slot slot;const char* extension;const char* field;uint32_t channel{4};};
constexpr std::array extensionSlots{
    ExtensionSlot{Slot::Specular,"KHR_materials_specular","specularTexture"},ExtensionSlot{Slot::SpecularColor,"KHR_materials_specular","specularColorTexture"},
    ExtensionSlot{Slot::Transmission,"KHR_materials_transmission","transmissionTexture"},ExtensionSlot{Slot::Clearcoat,"KHR_materials_clearcoat","clearcoatTexture"},
    ExtensionSlot{Slot::ClearcoatRoughness,"KHR_materials_clearcoat","clearcoatRoughnessTexture"},ExtensionSlot{Slot::ClearcoatNormal,"KHR_materials_clearcoat","clearcoatNormalTexture"},
    ExtensionSlot{Slot::Thickness,"KHR_materials_volume","thicknessTexture"},ExtensionSlot{Slot::SheenColor,"KHR_materials_sheen","sheenColorTexture"},
    ExtensionSlot{Slot::SheenRoughness,"KHR_materials_sheen","sheenRoughnessTexture"},ExtensionSlot{Slot::Iridescence,"KHR_materials_iridescence","iridescenceTexture"},
    ExtensionSlot{Slot::IridescenceThickness,"KHR_materials_iridescence","iridescenceThicknessTexture"},ExtensionSlot{Slot::Anisotropy,"KHR_materials_anisotropy","anisotropyTexture"},
    ExtensionSlot{Slot::DiffuseTransmission,"KHR_materials_diffuse_transmission","diffuseTransmissionTexture",3},
    ExtensionSlot{Slot::DiffuseTransmissionColor,"KHR_materials_diffuse_transmission","diffuseTransmissionColorTexture"}};
struct LegacySlot {Slot slot;const char* name;};
constexpr std::array legacySlots{LegacySlot{Slot::Roughness,"roughnessTexture"},LegacySlot{Slot::Metalness,"metalnessTexture"},
    LegacySlot{Slot::Specular,"specularTexture"},LegacySlot{Slot::Height,"heightTexture"},LegacySlot{Slot::Height,"displacementTexture"},
    LegacySlot{Slot::Opacity,"opacityTexture"},LegacySlot{Slot::Transmission,"refractionTexture"},LegacySlot{Slot::Transmission,"transmissionTexture"}};
[[noreturn]] void fail(std::string_view reason) {throw std::invalid_argument("Runtime material authoring: "+std::string(reason));}
bool bits(float a,float b) noexcept {return std::bit_cast<uint32_t>(a)==std::bit_cast<uint32_t>(b);}
bool transformEqual(const TextureTransform& a,const TextureTransform& b) noexcept {
  return bits(a.offset.x,b.offset.x) && bits(a.offset.y,b.offset.y) && bits(a.scale.x,b.scale.x) && bits(a.scale.y,b.scale.y) &&
      bits(a.rotation,b.rotation) && a.texCoord==b.texCoord && a.channel==b.channel;
}
const char* difference(const Material& a,const Material& b) noexcept {
  for(int i=0;i<4;++i) if(!bits(a.baseColor[i],b.baseColor[i])) return "baseColor";
  for(const auto& item:scalars) if(!bits(a.*item.member,b.*item.member)) return item.name;
  for(const auto& item:vectors) for(int i=0;i<3;++i) if(!bits((a.*item.member)[i],(b.*item.member)[i])) return item.name;
  if(a.alphaMode!=b.alphaMode) return "alphaMode";
  if(a.doubleSided!=b.doubleSided) return "doubleSided";
  if(a.specularGlossinessWorkflow!=b.specularGlossinessWorkflow) return "specularGlossinessWorkflow";
  if(a.unlit!=b.unlit) return "unlit";
  for(size_t i=0;i<count;++i) if(a.*resources[i]!=b.*resources[i] || !transformEqual(a.*transforms[i],b.*transforms[i])) return names[i];
  const auto& ax=a.experimental;const auto& bx=b.experimental;
  if(!bits(ax.retroreflection,bx.retroreflection) || !bits(ax.scatterStrength,bx.scatterStrength) ||
      !bits(ax.scatterAnisotropy,bx.scatterAnisotropy) || ax.textureResources()!=bx.textureResources() ||
      !transformEqual(ax.retroreflectionTransform,bx.retroreflectionTransform) ||
      !transformEqual(ax.scatterStrengthTransform,bx.scatterStrengthTransform) ||
      !transformEqual(ax.scatterColorTransform,bx.scatterColorTransform)) return "experimental material layers";
  for(int i=0;i<3;++i) if(!bits(ax.scatterColor[i],bx.scatterColor[i])) return "experimental scatter color";
  return nullptr;
}
int checked(int source,size_t textures) {
  if(source < -1 || (source>=0 && size_t(source)>=textures)) fail("source texture index is out of range");return source;
}
const Value* member(const Value&,std::string_view) noexcept;
int objectIndex(const Value& info,size_t textures) {
  const auto* index=member(info,"index");
  if(!info.IsObject() || !index || !index->IsInt() || index->Get<int>()<0)
    fail("known texture info requires an exact nonnegative integer index");
  return checked(index->Get<int>(),textures);
}
const Value* ext(const tinygltf::Material& m,const char* name) {
  for(const auto& [key,value]:m.extensions) if(std::string_view(key)==name) return &value;
  return nullptr;
}
const Value* member(const Value& value,std::string_view name) noexcept {
  if(!value.IsObject()) return nullptr;
  for(const auto& [key,item]:value.Get<Object>()) if(std::string_view(key)==name) return &item;
  return nullptr;
}
int exact(double value,const char* name) {
  if(!std::isfinite(value) || value<0 || value>INT_MAX || std::trunc(value)!=value) fail(name);return int(value);
}
void finiteConversionLimits(const GltfMaterialConversionLimits& limits) {
  const auto& validation=limits.validation;
  if(!limits.maxSourceTextures || limits.maxSourceTextures==UINT64_MAX || !limits.maxRuntimeResources || limits.maxRuntimeResources==UINT64_MAX)
    fail("conversion source/resource capacities must be finite and nonzero");
  if(validation.maxOutputBytes<=sizeof(ConvertedGltfMaterial) || validation.maxOutputBytes==UINT64_MAX)
    fail("conversion validation maxOutputBytes must have a finite usable capacity");
  if(!validation.maxMetadataValues || validation.maxMetadataValues==UINT64_MAX ||
      !validation.maxMetadataDepth || validation.maxMetadataDepth>256 ||
      !validation.maxAnimationChannels || validation.maxAnimationChannels==UINT64_MAX ||
      !validation.maxPatchOperations || validation.maxPatchOperations==UINT64_MAX)
    fail("conversion validation traversal capacities must be finite and nonzero");
}
void finiteRuntime(const Material& m) {
  for(int i=0;i<4;++i) if(!std::isfinite(m.baseColor[i])) fail("baseColor is nonfinite");
  for(const auto& item:scalars) {
    const auto value=m.*item.member;
    if(item.member==&Material::attenuationDistance && value==std::numeric_limits<float>::infinity()) continue;
    if(!std::isfinite(value)) fail(std::string(item.name)+" is nonfinite");
  }
  for(const auto& item:vectors) for(int i=0;i<3;++i) if(!std::isfinite((m.*item.member)[i])) fail(std::string(item.name)+" is nonfinite");
  if(m.alphaMode!=AlphaMode::Opaque && m.alphaMode!=AlphaMode::Mask && m.alphaMode!=AlphaMode::Blend) fail("unknown alphaMode");
  for(size_t i=0;i<count;++i) {
    const auto& t=m.*transforms[i];
    if(!std::isfinite(t.offset.x) || !std::isfinite(t.offset.y) || !std::isfinite(t.scale.x) || !std::isfinite(t.scale.y) || !std::isfinite(t.rotation))
      fail(std::string(names[i])+" transform is nonfinite");
    if(t.texCoord>1 || t.channel>4) fail(std::string(names[i])+" UV/channel is unsupported");
  }
}
void validateBindings(const Material& m,const RuntimeMaterialSourceTextures& ids,const GltfMaterialConversionContext& c,
    const GltfMaterialConversionLimits& limits) {
  if(!limits.maxSourceTextures || limits.maxSourceTextures==UINT64_MAX || !limits.maxRuntimeResources || limits.maxRuntimeResources==UINT64_MAX ||
      c.textureCount>limits.maxSourceTextures || c.textureCount>INT_MAX || c.runtimeResourceCount>limits.maxRuntimeResources ||
      c.runtimeResourceCount>UINT32_MAX || c.colorResources.size()!=c.textureCount || c.dataResources.size()!=c.textureCount)
    fail("source/resource table shape or finite limits are invalid");
  for(size_t i=0;i<count;++i) {
    const auto source=checked(ids[i],c.textureCount);const auto resource=m.*resources[i];
    if(source<0) {if(resource!=absent) fail(std::string(names[i])+" has no explicit source identity");continue;}
    const auto table=GltfMaterialTextureUsesColorResource(Slot(i),m)?c.colorResources:c.dataResources;
    if(resource==absent || resource>=c.runtimeResourceCount || table[size_t(source)]!=resource)
      fail(std::string(names[i])+" source role and resident resource disagree");
  }
}
Value array(glm::vec3 value) {return Value(Value::Array{Value(double(value.x)),Value(double(value.y)),Value(double(value.z))});}
Value array(glm::vec2 value) {return Value(Value::Array{Value(double(value.x)),Value(double(value.y))});}
Object& object(Value& value,const char* field) {
  if(value.Type()==tinygltf::NULL_TYPE) value=Value(Object{});
  if(!value.IsObject()) fail(std::string(field)+" retained metadata is not an object");return value.Get<Object>();
}
Object& extension(tinygltf::Material& m,const char* name) {return object(m.extensions[name],name);}
void textureTransform(tinygltf::ExtensionMap& extensions,const TextureTransform& transform) {
  auto& t=object(extensions["KHR_texture_transform"],"KHR_texture_transform");
  t["offset"]=array(transform.offset);t["scale"]=array(transform.scale);t["rotation"]=Value(double(transform.rotation));
  t.erase("texCoord");
}
template<class Info> void core(Info& info,Slot slot,const Material& m,const RuntimeMaterialSourceTextures& ids) {
  const auto n=size_t(slot);info.index=ids[n];
  if(info.index<0) return;
  const auto& t=m.*transforms[n];if(t.channel!=4) fail(std::string(names[n])+" channel has no standard glTF representation");
  info.texCoord=int(t.texCoord);textureTransform(info.extensions,t);info.extensions_json_string.clear();
}
void texture(Object& parent,const char* field,Slot slot,const Material& m,const RuntimeMaterialSourceTextures& ids,uint32_t channel=4) {
  const auto n=size_t(slot);if(ids[n]<0) {parent.erase(field);return;}
  const auto& t=m.*transforms[n];if(t.channel!=channel) fail(std::string(names[n])+" channel has no standard glTF representation");
  auto& info=object(parent[field],field);info["index"]=Value(ids[n]);info["texCoord"]=Value(int(t.texCoord));
  auto& extensions=object(info["extensions"],"texture extensions");
  auto& transform=object(extensions["KHR_texture_transform"],"KHR_texture_transform");
  transform["offset"]=array(t.offset);transform["scale"]=array(t.scale);transform["rotation"]=Value(double(t.rotation));transform.erase("texCoord");
}
void number(tinygltf::ParameterMap& values,const char* name,float value) {
  auto& p=values[name];p.has_number_value=true;p.number_value=double(value);
}
void legacy(tinygltf::Material& out,const Material& m,const RuntimeMaterialSourceTextures& ids) {
  for(auto* values:{&out.values,&out.additionalValues}) {
    number(*values,"opacity",m.opacityFactor);number(*values,"specular",m.specularFactor);number(*values,"refraction",m.transmissionFactor);
    number(*values,"heightScale",m.heightScale);number(*values,"heightOffset",m.heightOffset);
    for(const auto item:legacySlots) {
      const auto n=size_t(item.slot);if(ids[n]<0) {values->erase(item.name);continue;}
      auto& p=(*values)[item.name];const auto& t=m.*transforms[n];
      p.json_double_value["index"]=ids[n];p.json_double_value["texCoord"]=t.texCoord;p.json_double_value["channel"]=t.channel;
      p.json_double_value["offsetX"]=t.offset.x;p.json_double_value["offsetY"]=t.offset.y;
      p.json_double_value["scaleX"]=t.scale.x;p.json_double_value["scaleY"]=t.scale.y;p.json_double_value["rotation"]=t.rotation;
    }
  }
}
tinygltf::Material record(const Material& m,const RuntimeMaterialSourceTextures& ids,RuntimeMaterialAuthoringView view,
    const RuntimeMaterialAuthoringLimits& limits) {
  if(view.retained) {
    try {
      const auto converted=ConvertGltfMaterial(*view.retained,view.context,GltfMaterialConversionPolicy::StrictGltf,limits.conversion);
      if(RuntimeMaterialsEqual(converted.value,m) && converted.sourceTextures==ids.indices) return *view.retained;
    } catch(const std::invalid_argument&) {
      // A valid runtime origin may require canonicalization of retained import
      // fields. The selected copy below still retains unknown metadata; exact
      // forward conversion is mandatory before returning any result.
    }
  }
  if(m.specularGlossinessWorkflow && !view.retained) fail("specularGlossinessWorkflow needs an exact retained source definition");
  tinygltf::Material out=view.retained?*view.retained:tinygltf::Material{};
  out.extensions_json_string.clear();out.pbrMetallicRoughness.extensions_json_string.clear();
  out.pbrMetallicRoughness.baseColorFactor={double(m.baseColor.x),double(m.baseColor.y),double(m.baseColor.z),double(m.baseColor.w)};
  out.pbrMetallicRoughness.metallicFactor=m.metallicFactor;out.pbrMetallicRoughness.roughnessFactor=m.roughnessFactor;
  out.emissiveFactor={double(m.emissiveColor.x),double(m.emissiveColor.y),double(m.emissiveColor.z)};
  out.alphaMode=m.alphaMode==AlphaMode::Mask?"MASK":m.alphaMode==AlphaMode::Blend?"BLEND":"OPAQUE";
  out.alphaCutoff=m.alphaCutoff;out.doubleSided=m.doubleSided;
  core(out.normalTexture,Slot::Normal,m,ids);out.normalTexture.scale=m.normalTextureScale;
  core(out.occlusionTexture,Slot::Occlusion,m,ids);out.occlusionTexture.strength=m.occlusionStrength;
  core(out.emissiveTexture,Slot::Emissive,m,ids);
  if(m.specularGlossinessWorkflow) {
    auto& sg=extension(out,"KHR_materials_pbrSpecularGlossiness");
    texture(sg,"diffuseTexture",Slot::BaseColor,m,ids);
    if(ids[size_t(Slot::Roughness)]!=ids[size_t(Slot::SpecularColor)] ||
        !transformEqual(m.roughnessTextureTransform,m.specularColorTextureTransform))
      fail("specularGlossinessTexture requires one shared source identity and transform");
    texture(sg,"specularGlossinessTexture",Slot::Roughness,m,ids);sg["glossinessFactor"]=Value(double(m.roughnessFactor));
    // The non-invertible imported diffuse/specular factors remain source-owned.
    // The exact comparison below rejects attempts to reconstruct another pose.
  } else {
    out.extensions.erase("KHR_materials_pbrSpecularGlossiness");
    core(out.pbrMetallicRoughness.baseColorTexture,Slot::BaseColor,m,ids);
    core(out.pbrMetallicRoughness.metallicRoughnessTexture,Slot::MetallicRoughness,m,ids);
  }
  if(m.unlit) {
    extension(out,"KHR_materials_unlit");
    for(size_t e=1;e<size_t(geometry::gltf::GltfMaterialExtension::SpecularGlossiness);++e)
      out.extensions.erase(std::string(geometry::gltf::GltfMaterialExtensionName(geometry::gltf::GltfMaterialExtension(e))));
  } else out.extensions.erase("KHR_materials_unlit");
  if(!m.unlit && !m.specularGlossinessWorkflow) {
    extension(out,"KHR_materials_ior")["ior"]=Value(double(m.ior));
    extension(out,"KHR_materials_dispersion")["dispersion"]=Value(double(m.dispersion));
    extension(out,"KHR_materials_emissive_strength")["emissiveStrength"]=Value(double(m.emissiveStrength));
    auto& specular=extension(out,"KHR_materials_specular");specular["specularFactor"]=Value(double(m.specularFactor));specular["specularColorFactor"]=array(m.specularColorFactor);
    extension(out,"KHR_materials_transmission")["transmissionFactor"]=Value(double(m.transmissionFactor));
    auto& volume=extension(out,"KHR_materials_volume");volume["thicknessFactor"]=Value(double(m.thicknessFactor));volume["attenuationColor"]=array(m.attenuationColor);
    if(std::isfinite(m.attenuationDistance)) volume["attenuationDistance"]=Value(double(m.attenuationDistance));else volume.erase("attenuationDistance");
    auto& coat=extension(out,"KHR_materials_clearcoat");coat["clearcoatFactor"]=Value(double(m.clearcoatFactor));coat["clearcoatRoughnessFactor"]=Value(double(m.clearcoatRoughnessFactor));
    auto& sheen=extension(out,"KHR_materials_sheen");sheen["sheenColorFactor"]=array(m.sheenColorFactor);sheen["sheenRoughnessFactor"]=Value(double(m.sheenRoughnessFactor));
    auto& iridescence=extension(out,"KHR_materials_iridescence");iridescence["iridescenceFactor"]=Value(double(m.iridescenceFactor));iridescence["iridescenceIor"]=Value(double(m.iridescenceIor));
    iridescence["iridescenceThicknessMinimum"]=Value(double(m.iridescenceThicknessMinimum));iridescence["iridescenceThicknessMaximum"]=Value(double(m.iridescenceThicknessMaximum));
    auto& anisotropy=extension(out,"KHR_materials_anisotropy");anisotropy["anisotropyStrength"]=Value(double(m.anisotropyStrength));anisotropy["anisotropyRotation"]=Value(double(m.anisotropyRotation));
    auto& diffuse=extension(out,"KHR_materials_diffuse_transmission");diffuse["diffuseTransmissionFactor"]=Value(double(m.diffuseTransmissionFactor));diffuse["diffuseTransmissionColorFactor"]=array(m.diffuseTransmissionColorFactor);
    for(const auto item:extensionSlots) texture(extension(out,item.extension),item.field,item.slot,m,ids,item.channel);
    if(ids[size_t(Slot::ClearcoatNormal)]>=0)
      extension(out,"KHR_materials_clearcoat").at("clearcoatNormalTexture").Get<Object>()["scale"]=Value(double(m.clearcoatNormalTextureScale));
  } else if(m.specularGlossinessWorkflow) {
    extension(out,"KHR_materials_emissive_strength")["emissiveStrength"]=Value(double(m.emissiveStrength));
  }
  legacy(out,m,ids);
  return out;
}
void declarations(std::vector<std::string>& result,const tinygltf::Material& m,RuntimeMaterialAuthoringView a,RuntimeMaterialAuthoringView e) {
  const auto add=[&](std::string_view name) {
    if(std::ranges::find(a.context.extensionsUsed,name)!=a.context.extensionsUsed.end() &&
        std::ranges::find(e.context.extensionsUsed,name)!=e.context.extensionsUsed.end()) return;
    if(std::ranges::find(result,name)==result.end()) result.emplace_back(name);
  };
  for(const auto& [name,_]:m.extensions) add(name);
  bool transform=false;
  for(const auto* info:{&m.pbrMetallicRoughness.baseColorTexture,&m.pbrMetallicRoughness.metallicRoughnessTexture,&m.emissiveTexture})
    transform|=info->index>=0 && info->extensions.contains("KHR_texture_transform");
  transform|=(m.normalTexture.index>=0 && m.normalTexture.extensions.contains("KHR_texture_transform")) ||
      (m.occlusionTexture.index>=0 && m.occlusionTexture.extensions.contains("KHR_texture_transform"));
  for(const auto item:extensionSlots) if(const auto* x=ext(m,item.extension);x && x->IsObject() && x->Has(item.field)) {
    const auto& info=x->Get(item.field);transform|=info.Has("extensions") && info.Get("extensions").IsObject() && info.Get("extensions").Has("KHR_texture_transform");
  }
  if(const auto* sg=ext(m,"KHR_materials_pbrSpecularGlossiness");sg && sg->IsObject())
    for(const auto* key:{"diffuseTexture","specularGlossinessTexture"}) if(sg->Has(key)) {
      const auto& info=sg->Get(key);transform|=info.Has("extensions") && info.Get("extensions").IsObject() && info.Get("extensions").Has("KHR_texture_transform");
    }
  if(transform) add("KHR_texture_transform");
}
} // namespace

bool RuntimeMaterialsEqual(const Material& a,const Material& b) noexcept {return difference(a,b)==nullptr;}

RuntimeMaterialSourceTextures ReadGltfMaterialSourceTextures(const tinygltf::Material& m,size_t textures,
    const GltfMaterialConversionLimits& limits) {
  finiteConversionLimits(limits);
  if(!limits.maxSourceTextures || limits.maxSourceTextures==UINT64_MAX || textures>limits.maxSourceTextures || textures>INT_MAX)
    fail("source texture count exceeds its finite limit");
  (void)geometry::gltf::GltfMaterialRecordStorageBytes(m,limits.validation);
  RuntimeMaterialSourceTextures result;
  (void)checked(m.pbrMetallicRoughness.baseColorTexture.index,textures);
  (void)checked(m.pbrMetallicRoughness.metallicRoughnessTexture.index,textures);
  const auto set=[&](Slot slot,int index) {result[size_t(slot)]=checked(index,textures);};
  set(Slot::Normal,m.normalTexture.index);set(Slot::Occlusion,m.occlusionTexture.index);set(Slot::Emissive,m.emissiveTexture.index);
  const auto selected=[&](Slot slot,const Value* parent,const char* name) {
    if(!parent) return;if(!parent->IsObject()) fail("known material extension is not an object");
    if(const auto* info=member(*parent,name)) set(slot,objectIndex(*info,textures));
  };
  if(const auto* sg=ext(m,"KHR_materials_pbrSpecularGlossiness")) {
    selected(Slot::BaseColor,sg,"diffuseTexture");selected(Slot::Roughness,sg,"specularGlossinessTexture");selected(Slot::SpecularColor,sg,"specularGlossinessTexture");
  } else {set(Slot::BaseColor,m.pbrMetallicRoughness.baseColorTexture.index);set(Slot::MetallicRoughness,m.pbrMetallicRoughness.metallicRoughnessTexture.index);}
  for(const auto item:extensionSlots) selected(item.slot,ext(m,item.extension),item.field);
  for(const auto item:legacySlots) for(const auto* map:{&m.values,&m.additionalValues}) {
    const tinygltf::Parameter* found=nullptr;
    for(const auto& [name,value]:*map) if(std::string_view(name)==item.name) {found=&value;break;}
    if(!found) continue;
    const double* index=nullptr;
    for(const auto& [name,value]:found->json_double_value) if(std::string_view(name)=="index") {index=&value;break;}
    if(!index) fail("typed legacy texture has no index");
    const auto id=checked(exact(*index,"typed legacy texture index is invalid"),textures);
    if(result[size_t(item.slot)]<0) result[size_t(item.slot)]=id;
  }
  return result;
}

RuntimeMaterialAuthoringResult MaterializeRuntimeMaterial(const Material& m,const RuntimeMaterialSourceTextures& authoredIds,
    const RuntimeMaterialSourceTextures& evaluatedIds,RuntimeMaterialAuthoringView authored,RuntimeMaterialAuthoringView evaluated,
    const RuntimeMaterialAuthoringLimits& limits) {
  if(!limits.maxOutputBytes || limits.maxOutputBytes==UINT64_MAX) fail("output byte limit must be finite and nonzero");
  finiteConversionLimits(limits.conversion);
  // Runtime-only imports may retain their original record only in the archive.
  // Reuse it as an opaque metadata template for the evaluated record, then
  // rewrite every known binding from explicit evaluated IDs before validation.
  if(!evaluated.retained) evaluated.retained=authored.retained;
  finiteRuntime(m);validateBindings(m,authoredIds,authored.context,limits.conversion);validateBindings(m,evaluatedIds,evaluated.context,limits.conversion);
  uint64_t available=limits.maxOutputBytes;
  const auto charge=[&](uint64_t bytes) {if(bytes>available) fail("selected-record coexistence exceeds its output budget");available-=bytes;};
  charge(sizeof(RuntimeMaterialAuthoringResult)+2ull*1024*1024);
  for(const auto view:{authored,evaluated}) {
    if(view.context.extensionsUsed.size()>limits.conversion.validation.maxMetadataValues) fail("declaration traversal exceeds its finite limit");
    for(const auto& name:view.context.extensionsUsed) charge(sizeof(std::string)+name.size()+64);
    if(view.retained) {
      const auto bytes=geometry::gltf::GltfMaterialRecordStorageBytes(*view.retained,limits.conversion.validation);
      if(bytes>available/10) fail("retained selected-record coexistence exceeds its output budget");charge(bytes*10);
    }
  }
  RuntimeMaterialAuthoringResult result;
  result.authored_=record(m,authoredIds,authored,limits);result.evaluated_=record(m,evaluatedIds,evaluated,limits);
  for(const auto pair:{std::pair{&result.authored_,authored},std::pair{&result.evaluated_,evaluated}}) {
    const auto converted=ConvertGltfMaterial(*pair.first,pair.second.context,GltfMaterialConversionPolicy::StrictGltf,limits.conversion);
    if(const auto* name=difference(converted.value,m)) fail(std::string(name)+" has no exact source representation for this material");
  }
  if(ReadGltfMaterialSourceTextures(result.authored_,authored.context.textureCount,limits.conversion)!=authoredIds ||
      ReadGltfMaterialSourceTextures(result.evaluated_,evaluated.context.textureCount,limits.conversion)!=evaluatedIds)
    fail("canonical source slots differ from their explicit identities");
  declarations(result.declarations_,result.authored_,authored,evaluated);declarations(result.declarations_,result.evaluated_,authored,evaluated);
  result.bytes_=sizeof(result)+geometry::gltf::GltfMaterialRecordStorageBytes(result.authored_,limits.conversion.validation)+
      geometry::gltf::GltfMaterialRecordStorageBytes(result.evaluated_,limits.conversion.validation);
  for(const auto& name:result.declarations_) result.bytes_+=sizeof(name)+name.size()+64;
  if(result.bytes_>limits.maxOutputBytes) fail("result exceeds its output budget");
  return result;
}
} // namespace container::material
