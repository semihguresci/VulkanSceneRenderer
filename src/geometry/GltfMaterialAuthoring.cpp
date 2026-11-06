#include <Container/geometry/GltfMaterialAuthoring.h>

#include <Container/geometry/GltfAnimationRuntime.h>
#include <Container/geometry/GltfExtendedMaterials.h>
#include <Container/geometry/GltfMaterialExport.h>
#include <Container/utility/SceneNodeAuthoringPolicy.h>

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <climits>
#include <limits>
#include <set>
#include <stdexcept>
#include <type_traits>

namespace container::geometry::gltf {
struct GltfMaterialEditOwnership {
  std::array<bool, size_t(GltfMaterialProperty::Count)> numeric{};
  std::array<bool, size_t(GltfMaterialExtension::Count)> wholeExtensions{};
  std::array<uint8_t, size_t(GltfMaterialTextureSlot::Count)> textures{};
  bool alphaMode{}, doubleSided{}, name{};
};
class GltfMaterialAuthoringReceipt {
 public:
  GltfMaterialAuthoringPlan expected;
  GltfMaterialPatch patch;
  // Endpoint presence alone loses ownership across disable -> enable. These
  // fixed masks also allow a flat cumulative receipt without a preview chain.
  GltfMaterialEditOwnership ownership;
  size_t authoredTextures{}, evaluatedTextures{};
  size_t afterAuthoredTextures{}, afterEvaluatedTextures{};
  bool resourceTransition{};
  // Relevant pre-existing declarations must survive publication/replay.
  // Additional global declarations are harmless and intentionally permitted.
  std::vector<std::string> authoredDeclarations, evaluatedDeclarations;
  std::vector<std::string> afterAuthoredDeclarations, afterEvaluatedDeclarations;
  // A rebased plan still refers to the original immutable command, so repeated
  // replay never derives old rest/default values from a current sampled pose.
  std::shared_ptr<const GltfMaterialAuthoringReceipt> command;
};
struct GltfMaterialAuthoringAccess {
  static const auto& receipt(const GltfMaterialAuthoringPlan& plan) { return plan.receipt_; }
  static void publish(GltfMaterialAuthoringPlan& plan,
      std::shared_ptr<const GltfMaterialAuthoringReceipt> receipt) { plan.receipt_=std::move(receipt); }
};
namespace {
using P = GltfMaterialProperty;
using E = GltfMaterialExtension;
using V = tinygltf::Value;
using O = V::Object;
constexpr std::array<std::string_view, size_t(E::Count)> kExtensions{
    "KHR_materials_unlit", "KHR_materials_emissive_strength", "KHR_materials_ior", "KHR_materials_dispersion",
    "KHR_materials_transmission", "KHR_materials_volume", "KHR_materials_clearcoat", "KHR_materials_sheen",
    "KHR_materials_specular", "KHR_materials_iridescence", "KHR_materials_anisotropy",
    "KHR_materials_diffuse_transmission", "KHR_materials_pbrSpecularGlossiness"};
constexpr std::array<GltfMaterialPropertyInfo, size_t(P::Count)> kProperties{{
    {P::BaseColor,"pbrMetallicRoughness/baseColorFactor",4,{}}, {P::Metallic,"pbrMetallicRoughness/metallicFactor",1,{}},
    {P::Roughness,"pbrMetallicRoughness/roughnessFactor",1,{}}, {P::Emissive,"emissiveFactor",3,{}},
    {P::AlphaCutoff,"alphaCutoff",1,{}}, {P::NormalScale,"normalTexture/scale",1,{}}, {P::OcclusionStrength,"occlusionTexture/strength",1,{}},
    {P::EmissiveStrength,"extensions/KHR_materials_emissive_strength/emissiveStrength",1,E::EmissiveStrength},
    {P::Ior,"extensions/KHR_materials_ior/ior",1,E::Ior}, {P::Dispersion,"extensions/KHR_materials_dispersion/dispersion",1,E::Dispersion},
    {P::Transmission,"extensions/KHR_materials_transmission/transmissionFactor",1,E::Transmission},
    {P::Thickness,"extensions/KHR_materials_volume/thicknessFactor",1,E::Volume},
    {P::AttenuationDistance,"extensions/KHR_materials_volume/attenuationDistance",1,E::Volume},
    {P::AttenuationColor,"extensions/KHR_materials_volume/attenuationColor",3,E::Volume},
    {P::Clearcoat,"extensions/KHR_materials_clearcoat/clearcoatFactor",1,E::Clearcoat},
    {P::ClearcoatRoughness,"extensions/KHR_materials_clearcoat/clearcoatRoughnessFactor",1,E::Clearcoat},
    {P::ClearcoatNormalScale,"extensions/KHR_materials_clearcoat/clearcoatNormalTexture/scale",1,E::Clearcoat},
    {P::SheenColor,"extensions/KHR_materials_sheen/sheenColorFactor",3,E::Sheen},
    {P::SheenRoughness,"extensions/KHR_materials_sheen/sheenRoughnessFactor",1,E::Sheen},
    {P::Specular,"extensions/KHR_materials_specular/specularFactor",1,E::Specular},
    {P::SpecularColor,"extensions/KHR_materials_specular/specularColorFactor",3,E::Specular},
    {P::Iridescence,"extensions/KHR_materials_iridescence/iridescenceFactor",1,E::Iridescence},
    {P::IridescenceIor,"extensions/KHR_materials_iridescence/iridescenceIor",1,E::Iridescence},
    {P::IridescenceThicknessMinimum,"extensions/KHR_materials_iridescence/iridescenceThicknessMinimum",1,E::Iridescence},
    {P::IridescenceThicknessMaximum,"extensions/KHR_materials_iridescence/iridescenceThicknessMaximum",1,E::Iridescence},
    {P::Anisotropy,"extensions/KHR_materials_anisotropy/anisotropyStrength",1,E::Anisotropy},
    {P::AnisotropyRotation,"extensions/KHR_materials_anisotropy/anisotropyRotation",1,E::Anisotropy},
    {P::DiffuseTransmission,"extensions/KHR_materials_diffuse_transmission/diffuseTransmissionFactor",1,E::DiffuseTransmission},
    {P::DiffuseTransmissionColor,"extensions/KHR_materials_diffuse_transmission/diffuseTransmissionColorFactor",3,E::DiffuseTransmission},
    {P::SpecularGlossinessDiffuse,"extensions/KHR_materials_pbrSpecularGlossiness/diffuseFactor",4,E::SpecularGlossiness},
    {P::SpecularGlossinessSpecular,"extensions/KHR_materials_pbrSpecularGlossiness/specularFactor",3,E::SpecularGlossiness},
    {P::Glossiness,"extensions/KHR_materials_pbrSpecularGlossiness/glossinessFactor",1,E::SpecularGlossiness}}};
constexpr std::array kTextureFields{
    std::pair{E::Specular,"specularTexture"}, std::pair{E::Specular,"specularColorTexture"},
    std::pair{E::Transmission,"transmissionTexture"}, std::pair{E::Volume,"thicknessTexture"},
    std::pair{E::Clearcoat,"clearcoatTexture"}, std::pair{E::Clearcoat,"clearcoatRoughnessTexture"},
    std::pair{E::Clearcoat,"clearcoatNormalTexture"}, std::pair{E::Sheen,"sheenColorTexture"},
    std::pair{E::Sheen,"sheenRoughnessTexture"}, std::pair{E::Iridescence,"iridescenceTexture"},
    std::pair{E::Iridescence,"iridescenceThicknessTexture"}, std::pair{E::Anisotropy,"anisotropyTexture"},
    std::pair{E::DiffuseTransmission,"diffuseTransmissionTexture"}, std::pair{E::DiffuseTransmission,"diffuseTransmissionColorTexture"},
    std::pair{E::SpecularGlossiness,"diffuseTexture"}, std::pair{E::SpecularGlossiness,"specularGlossinessTexture"}};

[[noreturn]] void fail(std::string_view reason) { throw std::invalid_argument("glTF material authoring: "+std::string(reason)); }
const auto& info(P property) {
  if(size_t(property)>=kProperties.size()) fail("unknown typed property");
  return kProperties[size_t(property)];
}
std::string extensionName(E extension) {
  if(size_t(extension)>=kExtensions.size()) fail("unknown typed extension");
  return std::string(kExtensions[size_t(extension)]);
}
const V* ext(const tinygltf::Material& material,E extension) {
  const auto found=material.extensions.find(extensionName(extension));
  return found==material.extensions.end()?nullptr:&found->second;
}
bool has(const tinygltf::Material& material,E extension) { return ext(material,extension)!=nullptr; }
bool validValueStorage(const V& v) noexcept {
  return (v.IsString() || v.Get<std::string>().empty()) &&
      (v.IsBinary() || v.Get<std::vector<unsigned char>>().empty()) &&
      (v.IsArray() || v.Get<V::Array>().empty()) &&
      (v.IsObject() || v.Get<O>().empty()) &&
      (v.IsString() || v.IsBinary() || v.IsArray() || v.IsObject() ||
        v.IsBool() || v.IsNumber() || v.Type()==tinygltf::NULL_TYPE);
}
bool exactValue(const V& a,const V& b) {
  if(a.Type()!=b.Type() || !validValueStorage(a) || !validValueStorage(b)) return false;
  if(a.IsInt()) return a.Get<int>()==b.Get<int>();
  if(a.IsReal()) return std::bit_cast<uint64_t>(a.Get<double>())==std::bit_cast<uint64_t>(b.Get<double>());
  if(a.IsBool()) return a.Get<bool>()==b.Get<bool>();
  if(a.IsString()) return a.Get<std::string>()==b.Get<std::string>();
  if(a.IsBinary()) return a.Get<std::vector<unsigned char>>()==b.Get<std::vector<unsigned char>>();
  if(a.IsArray()) {
    const auto& x=a.Get<V::Array>();const auto& y=b.Get<V::Array>();if(x.size()!=y.size()) return false;
    for(size_t i=0;i<x.size();++i) if(!exactValue(x[i],y[i])) return false;
  } else if(a.IsObject()) {
    const auto& x=a.Get<O>();const auto& y=b.Get<O>();if(x.size()!=y.size()) return false;
    auto next=y.begin();for(const auto& [key,value]:x) {if(key!=next->first || !exactValue(value,next->second)) return false;++next;}
  }
  return true;
}
// TinyGLTF operator== deliberately approximates doubles. Exact authored source
// records, one-ULP edits, signed zero and optional leaf types are significant.
template<class T> bool exact(const T& a,const T& b) {
  if constexpr(std::is_same_v<T,V>) return exactValue(a,b);
  else if constexpr(std::is_same_v<T,double>) return std::bit_cast<uint64_t>(a)==std::bit_cast<uint64_t>(b);
  else if constexpr(std::is_arithmetic_v<T> || std::is_enum_v<T> || std::is_same_v<T,std::string>) return a==b;
  else if constexpr(requires {typename T::mapped_type;}) {
    if(a.size()!=b.size()) return false;auto next=b.begin();
    for(const auto& [key,value]:a) {if(key!=next->first || !exact(value,next->second)) return false;++next;}return true;
  } else if constexpr(requires {typename T::value_type;a.begin();}) {
    if(a.size()!=b.size()) return false;for(size_t i=0;i<a.size();++i) if(!exact(a[i],b[i])) return false;return true;
  } else {
    static_assert(requires {a.extras;} || std::is_same_v<T,tinygltf::Parameter>);
#define MATERIAL_EXACT_FIELD(field) if constexpr(requires {a.field;}) if(!exact(a.field,b.field)) return false
    MATERIAL_EXACT_FIELD(name);MATERIAL_EXACT_FIELD(extras);MATERIAL_EXACT_FIELD(extensions);
    MATERIAL_EXACT_FIELD(extras_json_string);MATERIAL_EXACT_FIELD(extensions_json_string);
    MATERIAL_EXACT_FIELD(pbrMetallicRoughness);MATERIAL_EXACT_FIELD(baseColorFactor);MATERIAL_EXACT_FIELD(metallicFactor);
    MATERIAL_EXACT_FIELD(roughnessFactor);MATERIAL_EXACT_FIELD(baseColorTexture);MATERIAL_EXACT_FIELD(metallicRoughnessTexture);
    MATERIAL_EXACT_FIELD(normalTexture);MATERIAL_EXACT_FIELD(occlusionTexture);MATERIAL_EXACT_FIELD(emissiveTexture);
    MATERIAL_EXACT_FIELD(emissiveFactor);MATERIAL_EXACT_FIELD(alphaMode);MATERIAL_EXACT_FIELD(alphaCutoff);
    MATERIAL_EXACT_FIELD(doubleSided);MATERIAL_EXACT_FIELD(lods);MATERIAL_EXACT_FIELD(index);MATERIAL_EXACT_FIELD(texCoord);
    MATERIAL_EXACT_FIELD(scale);MATERIAL_EXACT_FIELD(strength);MATERIAL_EXACT_FIELD(values);MATERIAL_EXACT_FIELD(additionalValues);
    MATERIAL_EXACT_FIELD(bool_value);MATERIAL_EXACT_FIELD(has_number_value);MATERIAL_EXACT_FIELD(string_value);
    MATERIAL_EXACT_FIELD(number_array);MATERIAL_EXACT_FIELD(json_double_value);MATERIAL_EXACT_FIELD(number_value);
#undef MATERIAL_EXACT_FIELD
    return true;
  }
}
uint64_t sum(uint64_t a,uint64_t b) {
  if(b>UINT64_MAX-a) fail("byte/count arithmetic overflow");return a+b;
}
struct Budget {
  const GltfMaterialAuthoringLimits& limits;
  uint64_t bytes{},values{};
  void add(uint64_t n) { bytes=sum(bytes,n);if(bytes>limits.maxOutputBytes) fail("owned material storage exceeds its budget"); }
  void value(const V& v,uint32_t depth=0) {
    if(depth>limits.maxMetadataDepth || ++values>limits.maxMetadataValues) fail("material metadata exceeds its depth/value budget");
    // TinyGLTF copies every backing container, even for an inactive tag. Such
    // malformed resident storage must not bypass copy/traversal accounting.
    if(!validValueStorage(v))
      fail("material metadata has invalid or inactive backing storage");
    add(sizeof(V));
    if(v.IsString()) add(v.Get<std::string>().size());
    else if(v.IsBinary()) add(v.Get<std::vector<unsigned char>>().size());
    else if(v.IsNumber()) { if(!std::isfinite(v.GetNumberAsDouble())) fail("nonfinite material metadata"); }
    else if(v.IsArray()) for(const auto& child:v.Get<V::Array>()) value(child,depth+1);
    else if(v.IsObject()) for(const auto& [key,child]:v.Get<O>()) { add(sum(128,key.size()));value(child,depth+1); }
  }
  template<class T> void property(const T& p) {
    add(sum(p.extras_json_string.size(),p.extensions_json_string.size()));value(p.extras);
    for(const auto& [key,v]:p.extensions) {add(sum(128,key.size()));value(v);}
  }
  void material(const tinygltf::Material& m) {
    add(sizeof(m));add(sum(m.name.size(),m.alphaMode.size()));
    add(sum(m.emissiveFactor.size(),m.pbrMetallicRoughness.baseColorFactor.size())*sizeof(double));
    add(m.lods.size()*sizeof(int));property(m);property(m.pbrMetallicRoughness);
    property(m.pbrMetallicRoughness.baseColorTexture);property(m.pbrMetallicRoughness.metallicRoughnessTexture);
    property(m.normalTexture);property(m.occlusionTexture);property(m.emissiveTexture);
    for(const auto* map:{&m.values,&m.additionalValues}) for(const auto& [key,p]:*map) {
      add(sum(128,key.size()));add(p.string_value.size());add(p.number_array.size()*sizeof(double));
      for(const auto& [name,number]:p.json_double_value) {add(sum(128,name.size()));if(!std::isfinite(number)) fail("nonfinite legacy parameter");}
      for(const auto number:p.number_array) if(!std::isfinite(number)) fail("nonfinite legacy parameter");
      if(p.has_number_value && !std::isfinite(p.number_value)) fail("nonfinite legacy parameter");
    }
  }
};
void limitsValid(const GltfMaterialAuthoringLimits& limits) {
  if(!limits.maxOutputBytes || limits.maxOutputBytes==UINT64_MAX || !limits.maxMetadataDepth || limits.maxMetadataDepth>256 ||
      !limits.maxMetadataValues || limits.maxMetadataValues==UINT64_MAX || !limits.maxAnimationChannels || limits.maxAnimationChannels==UINT64_MAX ||
      !limits.maxPatchOperations || limits.maxPatchOperations==UINT64_MAX) fail("invalid finite authoring limits");
}
bool exactBindings(std::span<const GltfMaterialAnimationBinding> a,std::span<const GltfMaterialAnimationBinding> b) {
  if(a.size()!=b.size()) return false;
  for(size_t i=0;i<a.size();++i) if(a[i].clip!=b[i].clip || a[i].channel!=b[i].channel || a[i].property!=b[i].property ||
      a[i].path!=b[i].path || a[i].component!=b[i].component) return false;
  return true;
}
bool exactPlan(const GltfMaterialAuthoringPlan& a,const GltfMaterialAuthoringPlan& b) {
  return a.kind==b.kind && a.sourceMaterial==b.sourceMaterial && a.destinationMaterial==b.destinationMaterial &&
      a.originalMaterialCount==b.originalMaterialCount && exact(a.authoredBefore,b.authoredBefore) &&
      exact(a.authoredAfter,b.authoredAfter) && exact(a.evaluatedBefore,b.evaluatedBefore) && exact(a.evaluatedAfter,b.evaluatedAfter) &&
      a.touchedProperties==b.touchedProperties && a.touchedExtensions==b.touchedExtensions &&
      a.touchedTextures==b.touchedTextures &&
      exactBindings(a.animationBindings,b.animationBindings) && a.extensionsToDeclare==b.extensionsToDeclare &&
      a.changed==b.changed && a.peakOwnedBytes==b.peakOwnedBytes;
}
void chargePatch(Budget& budget,const GltfMaterialPatch& patch) {
  budget.add(sizeof(patch));
  budget.add(patch.values.size()*sizeof(GltfMaterialNumericEdit));
  budget.add(patch.extensions.size()*sizeof(GltfMaterialExtensionEdit));
  budget.add(patch.textures.size()*sizeof(GltfMaterialTextureEdit));
  if(patch.name) budget.add(patch.name->size());
}
void chargePlan(Budget& budget,const GltfMaterialAuthoringPlan& plan) {
  budget.material(plan.authoredBefore);budget.material(plan.authoredAfter);
  budget.material(plan.evaluatedBefore);budget.material(plan.evaluatedAfter);
  budget.add(plan.touchedProperties.size()*sizeof(P));budget.add(plan.touchedExtensions.size()*sizeof(E));
  budget.add(plan.touchedTextures.size()*sizeof(GltfMaterialTextureSlot));
  for(const auto& binding:plan.animationBindings) budget.add(sum(sizeof(binding),binding.path.size()));
  for(const auto& name:plan.extensionsToDeclare) budget.add(sum(sizeof(name),name.size()));
}
void chargeStrings(Budget& budget,std::span<const std::string> strings) {
  for(const auto& text:strings) budget.add(sum(sizeof(text),text.size()));
}
std::vector<double> components(const GltfMaterialNumericValue& value) {
  return std::visit([](const auto& typed)->std::vector<double> {
    using T=std::decay_t<decltype(typed)>;
    if constexpr(std::is_same_v<T,std::monostate>) return {};
    else if constexpr(std::is_same_v<T,double>) return {typed};
    else return {typed.begin(),typed.end()};
  },value);
}
double defaultScalar(P p) {
  switch(p) {
    case P::Metallic:case P::Roughness:case P::NormalScale:case P::OcclusionStrength:case P::EmissiveStrength:
    case P::ClearcoatNormalScale:case P::Specular:case P::Glossiness:return 1;
    case P::AlphaCutoff:return .5;
    case P::Ior:return 1.5;
    case P::IridescenceIor:return 1.3;
    case P::IridescenceThicknessMinimum:return 100;
    case P::IridescenceThicknessMaximum:return 400;
    case P::AttenuationDistance:return std::numeric_limits<double>::infinity();
    default:return 0;
  }
}
std::vector<double> defaults(P p) {
  const auto n=info(p).components;
  if(n==1) return {defaultScalar(p)};
  const bool zero=p==P::Emissive || p==P::SheenColor;
  return std::vector<double>(n,zero?0.:1.);
}
std::string leaf(P p) {const auto path=info(p).path;return std::string(path.substr(path.rfind('/')+1));}
const V* parent(const tinygltf::Material& m,P p) {
  const auto& d=info(p);if(!d.extension) return nullptr;
  const auto* result=ext(m,*d.extension);
  if(result && !result->IsObject()) fail("known material extension must be an object");
  if(result && p==P::ClearcoatNormalScale) {
    if(!result->Has("clearcoatNormalTexture")) return nullptr;
    result=&result->Get("clearcoatNormalTexture");
    if(!result->IsObject()) fail("clearcoat normal texture must be an object");
  }
  return result;
}
std::vector<double> read(const tinygltf::Material& m,P p) {
  switch(p) {
    case P::BaseColor:return m.pbrMetallicRoughness.baseColorFactor;
    case P::Metallic:return {m.pbrMetallicRoughness.metallicFactor};
    case P::Roughness:return {m.pbrMetallicRoughness.roughnessFactor};
    case P::Emissive:return m.emissiveFactor;
    case P::AlphaCutoff:return {m.alphaCutoff};
    case P::NormalScale:return {m.normalTexture.scale};
    case P::OcclusionStrength:return {m.occlusionTexture.strength};
    default:break;
  }
  const auto* object=parent(m,p);const auto key=leaf(p);
  if(!object || !object->Has(key)) return defaults(p);
  const auto& value=object->Get(key);
  if(info(p).components==1) {
    if(!value.IsNumber()) fail("material factor must be a numeric scalar");return {value.GetNumberAsDouble()};
  }
  if(!value.IsArray()) fail("material color must be an array");
  std::vector<double> result;
  for(const auto& component:value.Get<V::Array>()) {if(!component.IsNumber()) fail("material color component must be numeric");result.push_back(component.GetNumberAsDouble());}
  return result;
}
bool unit(P p) {
  switch(p) {
    case P::BaseColor:case P::Metallic:case P::Roughness:case P::Emissive:case P::OcclusionStrength:
    case P::Transmission:case P::AttenuationColor:case P::Clearcoat:case P::ClearcoatRoughness:case P::SheenColor:
    case P::SheenRoughness:case P::Specular:case P::SpecularColor:case P::Iridescence:case P::Anisotropy:
    case P::DiffuseTransmission:case P::DiffuseTransmissionColor:case P::SpecularGlossinessDiffuse:
    case P::SpecularGlossinessSpecular:case P::Glossiness:return true;
    default:return false;
  }
}
void numeric(P p,std::span<const double> values,bool omittedDefault=false) {
  if(values.size()!=info(p).components) fail("wrong factor/color component count");
  for(const auto v:values) {
    if(omittedDefault && p==P::AttenuationDistance && v==std::numeric_limits<double>::infinity()) continue;
    if(!std::isfinite(v) || std::abs(v)>std::numeric_limits<float>::max()) fail("material factor must be finite and within binary32 range");
    if(unit(p) && (v<0 || v>1)) fail("material factor/color is outside [0,1]");
    if(p==P::Ior && v!=0 && v<1) fail("ior must be zero or at least one");
    if(p==P::IridescenceIor && v<1) fail("iridescence IOR must be at least one");
    if(p==P::AttenuationDistance && (v<=0 || float(v)<=0)) fail("attenuation distance must be positive or omitted");
    if((p==P::AlphaCutoff || p==P::EmissiveStrength || p==P::Dispersion || p==P::Thickness ||
        p==P::IridescenceThicknessMinimum || p==P::IridescenceThicknessMaximum) && v<0) fail("material factor must be nonnegative");
  }
}
void textureTransform(const tinygltf::ExtensionMap& extensions,int coordinate,const std::vector<std::string>& declared) {
  if(coordinate<0) fail("negative texture coordinate");
  if(const auto found=extensions.find("KHR_texture_transform");found!=extensions.end()) {
    if(std::ranges::find(declared,"KHR_texture_transform")==declared.end() || !found->second.IsObject()) fail("undeclared/malformed texture transform");
    const auto& t=found->second;
    for(const auto* key:{"offset","scale"}) if(t.Has(key)) {
      const auto& v=t.Get(key);if(!v.IsArray() || v.ArrayLen()!=2) fail("malformed texture transform vector");
      for(const auto& c:v.Get<V::Array>()) if(!c.IsNumber() || !std::isfinite(c.GetNumberAsDouble()) || std::abs(c.GetNumberAsDouble())>std::numeric_limits<float>::max()) fail("nonfinite texture transform");
    }
    if(t.Has("rotation")) {const auto& r=t.Get("rotation");if(!r.IsNumber() || !std::isfinite(r.GetNumberAsDouble()) || std::abs(r.GetNumberAsDouble())>std::numeric_limits<float>::max()) fail("invalid texture rotation");}
    if(t.Has("texCoord")) {const auto& uv=t.Get("texCoord");if(!uv.IsInt() || uv.Get<int>()<0) fail("invalid transform UV index");coordinate=uv.Get<int>();}
  }
  if(coordinate>1) fail("only effective UV0/UV1 material coordinates are supported");
}
void texture(const V& object,size_t count,const std::vector<std::string>& declared) {
  if(!object.IsObject() || !object.Has("index") || !object.Get("index").IsInt() || object.Get("index").Get<int>()<0 || size_t(object.Get("index").Get<int>())>=count)
    fail("material texture references a missing exact integer index");
  int coord=0;
  if(object.Has("texCoord")) {if(!object.Get("texCoord").IsInt()) fail("texture UV selector must be an integer");coord=object.Get("texCoord").Get<int>();}
  tinygltf::ExtensionMap empty;
  if(object.Has("extensions")) {if(!object.Get("extensions").IsObject()) fail("texture extensions must be an object");textureTransform(object.Get("extensions").Get<O>(),coord,declared);}
  else textureTransform(empty,coord,declared);
}
template<class T> void coreTexture(const T& object,size_t count,const std::vector<std::string>& declared) {
  if(object.index < -1 || (object.index>=0 && size_t(object.index)>=count)) fail("core material texture index is out of range");
  if(object.index>=0) textureTransform(object.extensions,object.texCoord,declared);
}
std::vector<std::string> materialDeclarations(const tinygltf::Material& material) {
  tinygltf::Model witness;witness.materials.push_back(material);
  // The existing collector handles material and nested texture-info maps.
  // Opaque pbrMetallicRoughness extensions are also genuine extension maps.
  if(!material.pbrMetallicRoughness.extensions.empty()) {
    tinygltf::Material pbr;pbr.extensions=material.pbrMetallicRoughness.extensions;
    witness.materials.push_back(std::move(pbr));
  }
  appendMaterialExtensionDeclarations(witness);
  return std::move(witness.extensionsUsed);
}
void validate(const tinygltf::Material& material,GltfMaterialAuthoringSourceView source,bool permitDeclarations) {
  if(source.textureCount>INT_MAX) fail("texture table exceeds integer identity range");
  // Only the bounded known set is needed by validation. Never copy a large
  // unrelated declaration table or any texture/image resource objects.
  std::vector<std::string> declared;
  if(std::ranges::find(source.extensionsUsed,"KHR_texture_transform")!=source.extensionsUsed.end()) declared.emplace_back("KHR_texture_transform");
  if(permitDeclarations && declared.empty()) {
    const auto discovered=materialDeclarations(material);
    if(std::ranges::find(discovered,"KHR_texture_transform")!=discovered.end()) declared.emplace_back("KHR_texture_transform");
  }
  for(const auto name:kExtensions) if(std::ranges::find(source.extensionsUsed,name)!=source.extensionsUsed.end()) declared.emplace_back(name);
  for(size_t e=0;e<kExtensions.size();++e) if(has(material,E(e))) {
    if(!ext(material,E(e))->IsObject()) fail("known material extension is not an object");
    if(std::ranges::find(declared,kExtensions[e])==declared.end()) {
      if(!permitDeclarations) fail("known material extension is not declared in extensionsUsed");declared.emplace_back(kExtensions[e]);
    }
  }
  if(material.alphaMode!="OPAQUE" && material.alphaMode!="MASK" && material.alphaMode!="BLEND") fail("unsupported material alpha mode");
  const bool unlit=has(material,E::Unlit),sg=has(material,E::SpecularGlossiness);
  if(unlit && sg) fail("unlit and specular-glossiness are mutually exclusive workflows");
  for(size_t e=1;e<size_t(E::SpecularGlossiness);++e) if(has(material,E(e))) {
    if(unlit || (sg && E(e)!=E::EmissiveStrength)) fail("lighting extension excludes unlit/specular-glossiness");
  }
  for(const auto& property:kProperties) {
    if(property.extension && !has(material,*property.extension)) continue;
    const auto* object=parent(material,property.property);
    const bool absent=property.property==P::AttenuationDistance && (!object || !object->Has(leaf(property.property)));
    numeric(property.property,read(material,property.property),absent);
  }
  coreTexture(material.pbrMetallicRoughness.baseColorTexture,source.textureCount,declared);
  coreTexture(material.pbrMetallicRoughness.metallicRoughnessTexture,source.textureCount,declared);
  coreTexture(material.normalTexture,source.textureCount,declared);coreTexture(material.occlusionTexture,source.textureCount,declared);
  coreTexture(material.emissiveTexture,source.textureCount,declared);
  for(const auto& [extension,key]:kTextureFields) if(const auto* object=ext(material,extension);object && object->Has(key)) texture(object->Get(key),source.textureCount,declared);
  ValidateGltfMaterialTextureSourceReferences(material,source.textureCount);
  // Compact validation witness: original indices were checked above. The
  // extended-material gate needs table size only, not actual texture resources.
  tinygltf::Model extended;extended.extensionsUsed=declared;extended.textures.resize(1);extended.materials.push_back(material);
  for(const auto& [extension,key]:kTextureFields) if(extension==E::Anisotropy || extension==E::DiffuseTransmission) {
    auto found=extended.materials[0].extensions.find(extensionName(extension));
    if(found!=extended.materials[0].extensions.end() && found->second.Has(key)) found->second.Get<O>().at(key).Get<O>().at("index")=V(0);
  }
  (void)ParseGltfExtendedMaterials(extended);
  // Reuse renderer/export range contracts without normalization, texture
  // remapping, legacy texture-backed zero promotion, or SG approximation.
  auto probeSource=material;probeSource.extensions.erase(extensionName(E::SpecularGlossiness));
  container::material::Material probe;UpdateAnimatedMaterialProperties(probeSource,probe);
  probe.unlit=false;probe.specularGlossinessWorkflow=false;
  (void)exportMaterial(probe,{});
  // Reversed iridescence thickness endpoints are explicitly valid in Khronos
  // KHR_materials_iridescence; do not add an invented min<=max restriction.
}
void requireTextureForEdit(const tinygltf::Material& m,P p) {
  if((p==P::NormalScale && m.normalTexture.index<0) || (p==P::OcclusionStrength && m.occlusionTexture.index<0)) fail("factor edit requires an existing texture object");
  if(p==P::ClearcoatNormalScale) {
    const auto* object=parent(m,p);
    if(!object || !object->Has("index")) fail("clearcoat normal scale requires an existing normal texture");
  }
}
void set(tinygltf::Material& m,P p,const GltfMaterialNumericValue& value) {
  requireTextureForEdit(m,p);const bool reset=std::holds_alternative<std::monostate>(value);
  const auto values=reset?defaults(p):components(value);numeric(p,values,reset);
  switch(p) {
    case P::BaseColor:m.pbrMetallicRoughness.baseColorFactor=values;return;
    case P::Metallic:m.pbrMetallicRoughness.metallicFactor=values[0];return;
    case P::Roughness:m.pbrMetallicRoughness.roughnessFactor=values[0];return;
    case P::Emissive:m.emissiveFactor=values;return;
    case P::AlphaCutoff:m.alphaCutoff=values[0];return;
    case P::NormalScale:m.normalTexture.scale=values[0];return;
    case P::OcclusionStrength:m.occlusionTexture.strength=values[0];return;
    default:break;
  }
  const auto extension=extensionName(*info(p).extension);
  if(reset && !m.extensions.contains(extension)) return;
  if(!m.extensions.contains(extension)) m.extensions.emplace(extension,V(O{}));
  auto* object=&m.extensions.at(extension).Get<O>();
  if(p==P::ClearcoatNormalScale) object=&object->at("clearcoatNormalTexture").Get<O>();
  const auto key=leaf(p);
  if(reset) {if(!object->erase(key)) return;}
  else {
    V replacement;
    if(values.size()==1) replacement=V(values[0]);
    else {V::Array array;for(const auto c:values) array.emplace_back(c);replacement=V(std::move(array));}
    if(const auto previous=object->find(key);previous!=object->end() && exactValue(previous->second,replacement)) return;
    (*object)[key]=std::move(replacement);
  }
  m.extensions_json_string.clear();
}
std::optional<uint32_t> integer(std::string_view token) {
  if(token.empty()) return {};uint32_t result{};
  const auto parsed=std::from_chars(token.data(),token.data()+token.size(),result);
  if(parsed.ec!=std::errc{} || parsed.ptr!=token.data()+token.size()) return {};return result;
}
std::vector<GltfMaterialAnimationBinding> bindings(GltfMaterialAuthoringSourceView source,int32_t material,Budget& budget) {
  std::vector<GltfMaterialAnimationBinding> result;uint64_t channels{};
  for(size_t clip=0;clip<source.animations.size();++clip) for(size_t channel=0;channel<source.animations[clip].channels.size();++channel) {
    if(++channels>budget.limits.maxAnimationChannels) fail("animation channel traversal exceeds its budget");
    const auto& input=source.animations[clip].channels[channel];
    const auto found=input.target_extensions.find("KHR_animation_pointer");
    if(found==input.target_extensions.end()) continue;
    const auto& target=found->second;
    if(!target.IsObject() || !target.Has("pointer") || !target.Get("pointer").IsString()) fail("malformed animation pointer object");
    const auto& pointer=target.Get("pointer").Get<std::string>();
    constexpr std::string_view prefix="/materials/";
    if(!std::string_view(pointer).starts_with(prefix)) continue;
    const auto separator=pointer.find('/',prefix.size());if(separator==std::string::npos) fail("material animation pointer has no property");
    const auto index=integer(std::string_view(pointer).substr(prefix.size(),separator-prefix.size()));
    if(!index || *index>=source.materialCount) fail("material animation pointer has an invalid source ID");
    if(*index!=uint32_t(material)) continue;
    budget.add(sum(sizeof(GltfMaterialAnimationBinding),pointer.size()));
    GltfMaterialAnimationBinding binding;binding.clip=clip;binding.channel=channel;binding.path=pointer.substr(separator+1);
    if(const auto slash=binding.path.rfind('/');slash!=std::string::npos) if(const auto component=integer(std::string_view(binding.path).substr(slash+1))) {
      binding.component=*component;binding.path.resize(slash);
    }
    for(const auto& property:kProperties) if(binding.path==property.path) {
      binding.property=property.property;
      if(binding.component && *binding.component>=property.components) fail("material animation pointer component is out of range");
    }
    result.push_back(std::move(binding));
  }
  return result;
}
const GltfMaterialAuthoringReceipt& proved(const GltfMaterialAuthoringPlan& plan) {
  const auto& receipt=GltfMaterialAuthoringAccess::receipt(plan);
  if(!receipt || !exactPlan(plan,receipt->expected)) fail("missing or modified material plan receipt");
  return *receipt;
}
void validateContext(const GltfMaterialAuthoringPlan& plan,const GltfMaterialAuthoringReceipt& receipt,
    GltfMaterialAuthoringSourceView authored,GltfMaterialAuthoringSourceView evaluated,Budget& budget,bool after=false) {
  const auto& limits=budget.limits;
  if(authored.materialCount!=plan.originalMaterialCount || evaluated.materialCount!=plan.originalMaterialCount ||
      authored.textureCount!=(after?receipt.afterAuthoredTextures:receipt.authoredTextures) ||
      evaluated.textureCount!=(after?receipt.afterEvaluatedTextures:receipt.evaluatedTextures))
    fail("material receipt resource context changed");
  if(authored.extensionsUsed.size()>limits.maxMetadataValues || evaluated.extensionsUsed.size()>limits.maxMetadataValues)
    fail("declaration traversal exceeds its budget");
  const auto& authoredNames=after && receipt.resourceTransition?receipt.afterAuthoredDeclarations:receipt.authoredDeclarations;
  const auto& evaluatedNames=after && receipt.resourceTransition?receipt.afterEvaluatedDeclarations:receipt.evaluatedDeclarations;
  for(const auto& name:authoredNames)
    if(std::ranges::find(authored.extensionsUsed,name)==authored.extensionsUsed.end()) fail("authored declaration witness was removed");
  for(const auto& name:evaluatedNames)
    if(std::ranges::find(evaluated.extensionsUsed,name)==evaluated.extensionsUsed.end()) fail("evaluated declaration witness was removed");
  if(plan.peakOwnedBytes>limits.maxOutputBytes) fail("material receipt storage exceeds its budget");
  if(plan.sourceMaterial) {
    const auto a=bindings(authored,*plan.sourceMaterial,budget),e=bindings(evaluated,*plan.sourceMaterial,budget);
    if(!exactBindings(a,plan.animationBindings) || !exactBindings(e,plan.animationBindings)) fail("material receipt animation target context changed");
  }
}

// Publication must not reconstruct binding vectors, material probes or error
// strings. Keep a nonthrowing equivalent of the preparation storage walk so a
// stale public record is bounded before any recursive exact comparison.
struct MatchBudget {
  const GltfMaterialAuthoringLimits& limits;
  uint64_t bytes{},values{};
  bool add(uint64_t n) noexcept {
    if(n>limits.maxOutputBytes-bytes) return false;bytes+=n;return true;
  }
  bool array(uint64_t count,uint64_t stride) noexcept {
    return count<=UINT64_MAX/stride && add(count*stride);
  }
  bool value(const V& v,uint32_t depth=0) noexcept {
    if(depth>limits.maxMetadataDepth || values==limits.maxMetadataValues || !validValueStorage(v)) return false;
    ++values;if(!add(sizeof(V))) return false;
    if(v.IsString()) return add(v.Get<std::string>().size());
    if(v.IsBinary()) return add(v.Get<std::vector<unsigned char>>().size());
    if(v.IsNumber()) return std::isfinite(v.GetNumberAsDouble());
    if(v.IsArray()) {for(const auto& child:v.Get<V::Array>()) if(!value(child,depth+1)) return false;}
    else if(v.IsObject()) for(const auto& [key,child]:v.Get<O>())
      if(!add(128) || !add(key.size()) || !value(child,depth+1)) return false;
    return true;
  }
  template<class T> bool property(const T& p) noexcept {
    if(!add(p.extras_json_string.size()) || !add(p.extensions_json_string.size()) || !value(p.extras)) return false;
    for(const auto& [key,v]:p.extensions) if(!add(128) || !add(key.size()) || !value(v)) return false;
    return true;
  }
  bool material(const tinygltf::Material& m) noexcept {
    if(!add(sizeof(m)) || !add(m.name.size()) || !add(m.alphaMode.size()) ||
        !array(m.emissiveFactor.size(),sizeof(double)) || !array(m.pbrMetallicRoughness.baseColorFactor.size(),sizeof(double)) ||
        !array(m.lods.size(),sizeof(int)) || !property(m) || !property(m.pbrMetallicRoughness) ||
        !property(m.pbrMetallicRoughness.baseColorTexture) || !property(m.pbrMetallicRoughness.metallicRoughnessTexture) ||
        !property(m.normalTexture) || !property(m.occlusionTexture) || !property(m.emissiveTexture)) return false;
    for(const auto* map:{&m.values,&m.additionalValues}) for(const auto& [key,p]:*map) {
      if(!add(128) || !add(key.size()) || !add(p.string_value.size()) || !array(p.number_array.size(),sizeof(double))) return false;
      for(const auto& [name,number]:p.json_double_value)
        if(!add(128) || !add(name.size()) || !std::isfinite(number)) return false;
      for(const auto number:p.number_array) if(!std::isfinite(number)) return false;
      if(p.has_number_value && !std::isfinite(p.number_value)) return false;
    }
    return true;
  }
  bool plan(const GltfMaterialAuthoringPlan& p) noexcept {
    if(!material(p.authoredBefore) || !material(p.authoredAfter) || !material(p.evaluatedBefore) || !material(p.evaluatedAfter) ||
        !array(p.touchedProperties.size(),sizeof(P)) || !array(p.touchedExtensions.size(),sizeof(E)) ||
        !array(p.touchedTextures.size(),sizeof(GltfMaterialTextureSlot))) return false;
    for(const auto& binding:p.animationBindings) if(!add(sizeof(binding)) || !add(binding.path.size())) return false;
    return strings(p.extensionsToDeclare);
  }
  bool patch(const GltfMaterialPatch& p) noexcept {
    return add(sizeof(p)) && array(p.values.size(),sizeof(GltfMaterialNumericEdit)) &&
        array(p.extensions.size(),sizeof(GltfMaterialExtensionEdit)) && array(p.textures.size(),sizeof(GltfMaterialTextureEdit)) &&
        (!p.name || add(p.name->size()));
  }
  bool strings(std::span<const std::string> names) noexcept {
    for(const auto& name:names) if(!add(sizeof(name)) || !add(name.size())) return false;return true;
  }
  bool receipt(const GltfMaterialAuthoringReceipt& r) noexcept {
    return plan(r.expected) && patch(r.patch) && strings(r.authoredDeclarations) && strings(r.evaluatedDeclarations) &&
        strings(r.afterAuthoredDeclarations) && strings(r.afterEvaluatedDeclarations);
  }
};
bool matchLimits(const GltfMaterialAuthoringLimits& l) noexcept {
  return l.maxOutputBytes && l.maxOutputBytes!=UINT64_MAX && l.maxMetadataDepth && l.maxMetadataDepth<=256 &&
      l.maxMetadataValues && l.maxMetadataValues!=UINT64_MAX && l.maxAnimationChannels && l.maxAnimationChannels!=UINT64_MAX &&
      l.maxPatchOperations && l.maxPatchOperations!=UINT64_MAX;
}
bool matchBindings(GltfMaterialAuthoringSourceView source,int32_t material,
    std::span<const GltfMaterialAnimationBinding> expected,MatchBudget& budget) noexcept {
  if(source.animations.size()>budget.limits.maxAnimationChannels) return false;
  uint64_t channels{},extensionValues{};size_t next{};
  for(size_t clip=0;clip<source.animations.size();++clip) {
    const auto& current=source.animations[clip].channels;
    if(current.size()>budget.limits.maxAnimationChannels-channels) return false;channels+=current.size();
    for(size_t channel=0;channel<current.size();++channel) {
      const auto& extensions=current[channel].target_extensions;
      if(extensions.size()>budget.limits.maxMetadataValues-extensionValues) return false;extensionValues+=extensions.size();
      // ExtensionMap's comparator is not transparent; find(long literal) would
      // allocate a std::string even when this material has no matching target.
      const auto found=std::find_if(extensions.begin(),extensions.end(),
          [](const auto& entry) {return entry.first=="KHR_animation_pointer";});
      if(found==extensions.end()) continue;
      const auto& target=found->second;
      if(!target.IsObject() || !validValueStorage(target) ||
          target.Get<O>().size()>budget.limits.maxMetadataValues-extensionValues) return false;
      extensionValues+=target.Get<O>().size();
      const auto field=std::find_if(target.Get<O>().begin(),target.Get<O>().end(),
          [](const auto& entry) {return entry.first=="pointer";});
      if(field==target.Get<O>().end() || !field->second.IsString() || !validValueStorage(field->second)) return false;
      const std::string_view pointer=field->second.Get<std::string>();
      if(pointer.size()>budget.limits.maxOutputBytes) return false;
      constexpr std::string_view prefix="/materials/";
      if(!pointer.starts_with(prefix)) continue;
      const auto separator=pointer.find('/',prefix.size());if(separator==std::string_view::npos) return false;
      const auto index=integer(pointer.substr(prefix.size(),separator-prefix.size()));
      if(!index || *index>=source.materialCount) return false;
      if(*index!=uint32_t(material)) continue;
      if(!budget.add(sizeof(GltfMaterialAnimationBinding)) || !budget.add(pointer.size()) || next>=expected.size()) return false;
      auto path=pointer.substr(separator+1);std::optional<uint32_t> component;
      if(const auto slash=path.rfind('/');slash!=std::string_view::npos)
        if(const auto parsed=integer(path.substr(slash+1))) {component=*parsed;path=path.substr(0,slash);}
      std::optional<P> property;
      for(const auto& entry:kProperties) if(path==entry.path) {
        property=entry.property;if(component && *component>=entry.components) return false;
      }
      const auto& binding=expected[next++];
      if(binding.clip!=clip || binding.channel!=channel || binding.path!=path ||
          binding.component!=component || binding.property!=property) return false;
    }
  }
  return next==expected.size();
}
bool matchContext(const GltfMaterialAuthoringPlan& plan,const GltfMaterialAuthoringReceipt& receipt,
    GltfMaterialAuthoringSourceView authored,GltfMaterialAuthoringSourceView evaluated,MatchBudget& budget,bool after=false) noexcept {
  if(authored.materialCount!=plan.originalMaterialCount || evaluated.materialCount!=plan.originalMaterialCount ||
      authored.textureCount!=(after?receipt.afterAuthoredTextures:receipt.authoredTextures) ||
      evaluated.textureCount!=(after?receipt.afterEvaluatedTextures:receipt.evaluatedTextures) ||
      authored.extensionsUsed.size()>budget.limits.maxMetadataValues || evaluated.extensionsUsed.size()>budget.limits.maxMetadataValues ||
      plan.peakOwnedBytes>budget.limits.maxOutputBytes) return false;
  const auto& aNames=after && receipt.resourceTransition?receipt.afterAuthoredDeclarations:receipt.authoredDeclarations;
  const auto& eNames=after && receipt.resourceTransition?receipt.afterEvaluatedDeclarations:receipt.evaluatedDeclarations;
  for(const auto& name:aNames) if(std::ranges::find(authored.extensionsUsed,name)==authored.extensionsUsed.end()) return false;
  for(const auto& name:eNames) if(std::ranges::find(evaluated.extensionsUsed,name)==evaluated.extensionsUsed.end()) return false;
  return !plan.sourceMaterial || (matchBindings(authored,*plan.sourceMaterial,plan.animationBindings,budget) &&
      matchBindings(evaluated,*plan.sourceMaterial,plan.animationBindings,budget));
}
bool matchBefore(const GltfMaterialAuthoringPlan& plan,const tinygltf::Material& authored,
    const tinygltf::Material& evaluated,const GltfMaterialAuthoringLimits& limits) noexcept {
  MatchBudget current{limits};
  return current.material(authored) && current.material(evaluated) &&
      exact(authored,plan.authoredBefore) && exact(evaluated,plan.evaluatedBefore);
}
GltfMaterialEditOwnership ownership(const GltfMaterialAuthoringPlan& plan,const GltfMaterialPatch& patch) {
  GltfMaterialEditOwnership result;
  for(const auto& edit:patch.values) result.numeric[size_t(edit.property)]=true;
  for(const auto& edit:patch.textures) {
    const auto mask=GltfMaterialTextureEditOwnership(edit);result.textures[size_t(edit.slot)]|=mask;
    for(const auto* material:{&plan.authoredBefore,&plan.authoredAfter,&plan.evaluatedBefore,&plan.evaluatedAfter}) {
      const auto binding=ReadGltfMaterialTextureBinding(*material,edit.slot);
      if(binding.coupledSlot && (binding.index>=0 || edit.operation==GltfMaterialTextureOperation::Bind))
        result.textures[size_t(*binding.coupledSlot)]|=mask;
    }
  }
  for(size_t slot=0;slot<result.textures.size();++slot) if(result.textures[slot]&TextureBinding) {
    const auto textureSlot=GltfMaterialTextureSlot(slot);
    for(const auto [before,after]:{std::pair{&plan.authoredBefore,&plan.authoredAfter},std::pair{&plan.evaluatedBefore,&plan.evaluatedAfter}}) {
      const auto a=ReadGltfMaterialTextureBinding(*before,textureSlot),b=ReadGltfMaterialTextureBinding(*after,textureSlot);
      if((a.index<0)!=(b.index<0) || a.path!=b.path || a.legacy!=b.legacy) result.textures[slot]|=TextureContainer;
    }
  }
  for(size_t e=0;e<size_t(E::Count);++e)
    result.wholeExtensions[e]=has(plan.authoredBefore,E(e))!=has(plan.authoredAfter,E(e)) ||
        has(plan.evaluatedBefore,E(e))!=has(plan.evaluatedAfter,E(e));
  result.alphaMode=patch.alphaMode.has_value();result.doubleSided=patch.doubleSided.has_value();result.name=patch.name.has_value();
  return result;
}
void mint(GltfMaterialAuthoringPlan& result,const GltfMaterialPatch& patch,
    GltfMaterialAuthoringSourceView authored,GltfMaterialAuthoringSourceView evaluated,Budget& budget,
    const GltfMaterialEditOwnership& owned,
    std::shared_ptr<const GltfMaterialAuthoringReceipt> command={},
    std::optional<GltfMaterialAuthoringSourceView> afterAuthored={},
    std::optional<GltfMaterialAuthoringSourceView> afterEvaluated={}) {
  // The selected-record reservation was made before any material copies.
  // Declaration keys and the retained patch are separately bounded BEFORE
  // allocating the witness. Unrelated global declaration lists are borrowed.
  std::set<std::string,std::less<>> relevant;
  const auto addName=[&](std::string_view name) {
    if(relevant.contains(name)) return;
    budget.add(sum(128,name.size()));relevant.emplace(name);
  };
  for(const auto name:kExtensions) addName(name);
  addName("KHR_texture_transform");
  for(const auto* material:{&result.authoredBefore,&result.authoredAfter,&result.evaluatedBefore,&result.evaluatedAfter})
    for(const auto& name:materialDeclarations(*material)) addName(name);
  budget.add(sizeof(GltfMaterialAuthoringReceipt));chargePatch(budget,patch);
  std::vector<std::string> aDeclarations,eDeclarations,afterADeclarations,afterEDeclarations;
  for(const auto& name:relevant) {
    if(std::ranges::find(authored.extensionsUsed,name)!=authored.extensionsUsed.end()) {
      budget.add(sum(sizeof(std::string),name.size()));aDeclarations.push_back(name);
    }
    if(std::ranges::find(evaluated.extensionsUsed,name)!=evaluated.extensionsUsed.end()) {
      budget.add(sum(sizeof(std::string),name.size()));eDeclarations.push_back(name);
    }
    if(afterAuthored && std::ranges::find(afterAuthored->extensionsUsed,name)!=afterAuthored->extensionsUsed.end()) {
      budget.add(sum(sizeof(std::string),name.size()));afterADeclarations.push_back(name);
    }
    if(afterEvaluated && std::ranges::find(afterEvaluated->extensionsUsed,name)!=afterEvaluated->extensionsUsed.end()) {
      budget.add(sum(sizeof(std::string),name.size()));afterEDeclarations.push_back(name);
    }
  }
  result.peakOwnedBytes=budget.bytes;
  auto receipt=std::make_shared<GltfMaterialAuthoringReceipt>();
  receipt->expected=result;receipt->patch=patch;
  receipt->ownership=owned;
  receipt->authoredTextures=authored.textureCount;receipt->evaluatedTextures=evaluated.textureCount;
  receipt->afterAuthoredTextures=afterAuthored?afterAuthored->textureCount:authored.textureCount;
  receipt->afterEvaluatedTextures=afterEvaluated?afterEvaluated->textureCount:evaluated.textureCount;
  receipt->resourceTransition=afterAuthored.has_value();
  receipt->authoredDeclarations=std::move(aDeclarations);receipt->evaluatedDeclarations=std::move(eDeclarations);
  receipt->afterAuthoredDeclarations=std::move(afterADeclarations);receipt->afterEvaluatedDeclarations=std::move(afterEDeclarations);
  receipt->command=std::move(command);
  GltfMaterialAuthoringAccess::publish(result,std::move(receipt));
}
GltfMaterialAuthoringPlan plan(GltfMaterialAuthoringSourceView authored,GltfMaterialAuthoringSourceView evaluated,
    std::optional<int32_t> sourceId,const tinygltf::Material& a,const tinygltf::Material& e,
    const GltfMaterialPatch& patch,GltfMaterialPlanKind kind,const GltfMaterialAuthoringLimits& limits,
    std::optional<GltfMaterialAuthoringSourceView> prospectiveAuthored={},
    std::optional<GltfMaterialAuthoringSourceView> prospectiveEvaluated={}) {
  limitsValid(limits);
  const auto afterAuthored=prospectiveAuthored.value_or(authored),afterEvaluated=prospectiveEvaluated.value_or(evaluated);
  if(prospectiveAuthored.has_value()!=prospectiveEvaluated.has_value() ||
      afterAuthored.materialCount!=authored.materialCount || afterEvaluated.materialCount!=evaluated.materialCount ||
      authored.textureCount>INT_MAX || evaluated.textureCount>INT_MAX || afterAuthored.textureCount>INT_MAX || afterEvaluated.textureCount>INT_MAX)
    fail("invalid independent material resource transition domains");
  if(authored.extensionsUsed.size()>limits.maxMetadataValues || evaluated.extensionsUsed.size()>limits.maxMetadataValues ||
      afterAuthored.extensionsUsed.size()>limits.maxMetadataValues || afterEvaluated.extensionsUsed.size()>limits.maxMetadataValues)
    fail("declaration traversal exceeds its budget");
  if(authored.materialCount!=evaluated.materialCount || authored.materialCount>INT_MAX ||
      (kind!=GltfMaterialPlanKind::Edit && authored.materialCount==INT_MAX)) fail("material identity tables differ or cannot append");
  if(sourceId && (*sourceId<0 || size_t(*sourceId)>=authored.materialCount)) fail("source material ID is out of range");
  const auto operations=sum(sum(sum(patch.values.size(),patch.extensions.size()),patch.textures.size()),
      uint64_t(patch.alphaMode.has_value())+uint64_t(patch.doubleSided.has_value())+uint64_t(patch.name.has_value()));
  if(operations>limits.maxPatchOperations) fail("patch operation count exceeds its budget");
  if(patch.textures.size()>size_t(GltfMaterialTextureSlot::Count)) fail("texture edit count exceeds its registry");
  for(const auto& edit:patch.textures) GltfMaterialTextureSlotDescription(edit.slot);
  if(patch.name) container::scene::ValidateSceneNodeName(*patch.name);
  std::set<P> properties;std::set<E> extensions;std::set<E> disabled;
  for(const auto& edit:patch.values) {info(edit.property);if(!properties.insert(edit.property).second) fail("duplicate numeric property edit");
    if(!std::holds_alternative<std::monostate>(edit.value)) numeric(edit.property,components(edit.value));}
  for(const auto& edit:patch.extensions) {extensionName(edit.extension);if(!extensions.insert(edit.extension).second) fail("duplicate extension edit");if(!edit.enabled) disabled.insert(edit.extension);}
  for(const auto p:properties) if(info(p).extension && disabled.contains(*info(p).extension)) fail("numeric edit conflicts with disabled extension");
  if(patch.alphaMode && *patch.alphaMode!=GltfMaterialAlphaMode::Opaque && *patch.alphaMode!=GltfMaterialAlphaMode::Mask && *patch.alphaMode!=GltfMaterialAlphaMode::Blend) fail("unknown typed alpha mode");
  Budget budget{limits};
  // Four output snapshots plus transient validation/probe copies are charged
  // before the first material copy. Patch growth has a bounded conservative
  // reservation, including map/object allocation overhead and declarations.
  for(size_t copy=0;copy<6;++copy) {budget.material(a);budget.material(e);}
  if(kind==GltfMaterialPlanKind::MakeUnique) {budget.material(e);budget.material(e);}
  budget.add(sum(sum(patch.values.size(),patch.extensions.size()),patch.textures.size())*8192);
  if(patch.name) budget.add(patch.name->size()*8);
  auto tracked=sourceId?bindings(authored,*sourceId,budget):std::vector<GltfMaterialAnimationBinding>{};
  if(sourceId) {
    auto liveTracked=bindings(evaluated,*sourceId,budget);
    if(liveTracked.size()!=tracked.size()) fail("authored/evaluated animation binding tables differ");
    for(size_t i=0;i<tracked.size();++i) if(liveTracked[i].clip!=tracked[i].clip || liveTracked[i].channel!=tracked[i].channel ||
        liveTracked[i].path!=tracked[i].path || liveTracked[i].component!=tracked[i].component) fail("authored/evaluated animation pointer identities differ");
  }
  for(const auto extension:disabled) if(kind==GltfMaterialPlanKind::Edit) {
    const auto object="extensions/"+extensionName(extension),prefix=object+"/";
    if(std::ranges::any_of(tracked,[&](const auto& b) {return b.path==object || b.path.starts_with(prefix);})) fail("cannot disable an extension with animation targets");
  }
  validate(a,authored,kind==GltfMaterialPlanKind::Append);validate(e,evaluated,kind==GltfMaterialPlanKind::Append);
  GltfMaterialAuthoringPlan result;result.kind=kind;result.sourceMaterial=sourceId;result.originalMaterialCount=authored.materialCount;
  result.destinationMaterial=kind==GltfMaterialPlanKind::Edit?*sourceId:int32_t(authored.materialCount);
  result.authoredBefore=a;result.authoredAfter=kind==GltfMaterialPlanKind::MakeUnique?e:a;
  result.evaluatedBefore=e;result.evaluatedAfter=e;
  result.animationBindings=std::move(tracked);
  for(auto* material:{&result.authoredAfter,&result.evaluatedAfter}) {
    for(const auto& edit:patch.extensions) {
      const auto name=extensionName(edit.extension);
      bool changed=false;
      if(edit.enabled) {if(!material->extensions.contains(name)) {material->extensions.emplace(name,V(O{}));changed=true;}}
      else changed=material->extensions.erase(name)!=0;
      if(changed) material->extensions_json_string.clear();
    }
  }
  ValidateGltfMaterialTextureEdits(patch.textures,result.authoredAfter,result.evaluatedAfter,afterAuthored.textureCount,afterEvaluated.textureCount);
  for(const auto& edit:patch.textures) {
    if(edit.operation!=GltfMaterialTextureOperation::Clear && edit.operation!=GltfMaterialTextureOperation::Mapping) {
      for(const auto* material:{&result.authoredAfter,&result.evaluatedAfter}) {
        const auto name=ReadGltfMaterialTextureBinding(*material,edit.slot).extension;
        for(const auto extension:disabled) if(name==GltfMaterialExtensionName(extension)) fail("texture binding conflicts with disabled extension");
      }
    }
    if(edit.operation==GltfMaterialTextureOperation::Clear && kind==GltfMaterialPlanKind::Edit) {
      for(const auto* material:{&a,&e}) {
        const auto texturePath=ReadGltfMaterialTextureBinding(*material,edit.slot).path;
        if(std::ranges::any_of(result.animationBindings,[&](const auto& binding) {
          return binding.path==texturePath || (binding.path.starts_with(texturePath) && binding.path.size()>texturePath.size() && binding.path[texturePath.size()]=='/');
        })) fail("cannot clear a texture object with animation targets");
      }
    }
  }
  ApplyGltfMaterialTextureEdits(result.authoredAfter,patch.textures,false);
  ApplyGltfMaterialTextureEdits(result.evaluatedAfter,patch.textures,true);
  for(auto* material:{&result.authoredAfter,&result.evaluatedAfter}) {
    for(const auto& edit:patch.values) set(*material,edit.property,edit.value);
    if(patch.alphaMode) material->alphaMode=*patch.alphaMode==GltfMaterialAlphaMode::Mask?"MASK":*patch.alphaMode==GltfMaterialAlphaMode::Blend?"BLEND":"OPAQUE";
    if(patch.doubleSided) material->doubleSided=*patch.doubleSided;
    if(patch.name) material->name=*patch.name;
  }
  validate(result.authoredAfter,afterAuthored,true);validate(result.evaluatedAfter,afterEvaluated,true);
  result.touchedProperties.assign(properties.begin(),properties.end());
  for(const auto property:properties) if(info(property).extension) extensions.insert(*info(property).extension);
  const auto owned=ownership(result,patch);
  for(size_t slot=0;slot<owned.textures.size();++slot) if(owned.textures[slot]) {
    result.touchedTextures.push_back(GltfMaterialTextureSlot(slot));
    for(const auto* material:{&result.authoredBefore,&result.authoredAfter,&result.evaluatedBefore,&result.evaluatedAfter}) {
      const auto name=ReadGltfMaterialTextureBinding(*material,GltfMaterialTextureSlot(slot)).extension;
      for(size_t extension=0;extension<kExtensions.size();++extension) if(name==kExtensions[extension]) extensions.insert(E(extension));
    }
  }
  result.touchedExtensions.assign(extensions.begin(),extensions.end());
  std::set<std::string> declarations;
  for(const auto* material:{&result.authoredAfter,&result.evaluatedAfter})
    for(auto& name:materialDeclarations(*material)) declarations.insert(std::move(name));
  for(const auto& name:declarations) if(std::ranges::find(afterAuthored.extensionsUsed,name)==afterAuthored.extensionsUsed.end() ||
      std::ranges::find(afterEvaluated.extensionsUsed,name)==afterEvaluated.extensionsUsed.end()) {
    budget.add(sum(sizeof(std::string),name.size())*2);result.extensionsToDeclare.push_back(name);
  }
  result.changed=kind!=GltfMaterialPlanKind::Edit || !exact(result.authoredBefore,result.authoredAfter) || !exact(result.evaluatedBefore,result.evaluatedAfter);
  mint(result,patch,authored,evaluated,budget,owned,{},prospectiveAuthored,prospectiveEvaluated);
  return result;
}
const tinygltf::Material& selected(const tinygltf::Model& source,int32_t index) {
  if(index<0 || size_t(index)>=source.materials.size()) fail("source material ID is out of range");return source.materials[size_t(index)];
}
void copyProperty(tinygltf::Material& target,const tinygltf::Material& source,P property) {
  switch(property) {
    case P::BaseColor:target.pbrMetallicRoughness.baseColorFactor=source.pbrMetallicRoughness.baseColorFactor;return;
    case P::Metallic:target.pbrMetallicRoughness.metallicFactor=source.pbrMetallicRoughness.metallicFactor;return;
    case P::Roughness:target.pbrMetallicRoughness.roughnessFactor=source.pbrMetallicRoughness.roughnessFactor;return;
    case P::Emissive:target.emissiveFactor=source.emissiveFactor;return;
    case P::AlphaCutoff:target.alphaCutoff=source.alphaCutoff;return;
    case P::NormalScale:target.normalTexture.scale=source.normalTexture.scale;return;
    case P::OcclusionStrength:target.occlusionTexture.strength=source.occlusionTexture.strength;return;
    default:break;
  }
  const auto name=extensionName(*info(property).extension),key=leaf(property);
  const auto* original=parent(source,property);
  auto found=target.extensions.find(name);
  if(!original || !original->Has(key)) {
    if(found==target.extensions.end()) return;
    auto* object=&found->second.Get<O>();
    if(property==P::ClearcoatNormalScale) {
      const auto normal=object->find("clearcoatNormalTexture");if(normal==object->end()) return;
      object=&normal->second.Get<O>();
    }
    object->erase(key);return;
  }
  if(found==target.extensions.end()) found=target.extensions.emplace(name,V(O{})).first;
  auto* object=&found->second.Get<O>();
  if(property==P::ClearcoatNormalScale) {
    // This edit owns scale, never an unrelated texture binding/metadata.
    const auto normal=object->find("clearcoatNormalTexture");
    if(normal==object->end()) fail("current evaluated normal texture binding changed");
    object=&normal->second.Get<O>();
  }
  (*object)[key]=original->Get(key);
}
void declarations(GltfMaterialAuthoringPlan& result,GltfMaterialAuthoringSourceView authored,
    GltfMaterialAuthoringSourceView evaluated,Budget& budget) {
  std::set<std::string> names;
  for(const auto* material:{&result.authoredAfter,&result.evaluatedAfter})
    for(auto& name:materialDeclarations(*material)) names.insert(std::move(name));
  for(const auto& name:names) if(std::ranges::find(authored.extensionsUsed,name)==authored.extensionsUsed.end() ||
      std::ranges::find(evaluated.extensionsUsed,name)==evaluated.extensionsUsed.end()) {
    budget.add(sum(sizeof(std::string),name.size())*2);result.extensionsToDeclare.push_back(name);
  }
}
void chargeReceipt(Budget& budget,const GltfMaterialAuthoringReceipt& receipt) {
  chargePlan(budget,receipt.expected);chargePatch(budget,receipt.patch);
  chargeStrings(budget,receipt.authoredDeclarations);chargeStrings(budget,receipt.evaluatedDeclarations);
  chargeStrings(budget,receipt.afterAuthoredDeclarations);chargeStrings(budget,receipt.afterEvaluatedDeclarations);
}
void validateIntrinsicTransition(const GltfMaterialAuthoringPlan& plan,const GltfMaterialAuthoringReceipt& receipt,Budget& budget) {
  if(!receipt.resourceTransition || plan.kind!=GltfMaterialPlanKind::Edit || !plan.sourceMaterial ||
      plan.destinationMaterial!=*plan.sourceMaterial) fail("expected an existing-material resource-transition receipt");
  chargePlan(budget,plan);chargeReceipt(budget,receipt);
  if(receipt.command) chargeReceipt(budget,*receipt.command);
  const auto count=plan.originalMaterialCount;
  validate(plan.authoredBefore,{count,receipt.authoredTextures,receipt.authoredDeclarations,{}},true);
  validate(plan.evaluatedBefore,{count,receipt.evaluatedTextures,receipt.evaluatedDeclarations,{}},true);
  validate(plan.authoredAfter,{count,receipt.afterAuthoredTextures,receipt.afterAuthoredDeclarations,{}},true);
  validate(plan.evaluatedAfter,{count,receipt.afterEvaluatedTextures,receipt.afterEvaluatedDeclarations,{}},true);
  if(plan.peakOwnedBytes>budget.limits.maxOutputBytes) fail("material transition exceeds its retained budget");
}
void copyOwnedEvaluated(tinygltf::Material& target,const tinygltf::Material& current,
    const tinygltf::Material& destination,const GltfMaterialEditOwnership& owned) {
  for(size_t i=0;i<size_t(E::Count);++i) if(owned.wholeExtensions[i]) {
    const auto name=extensionName(E(i));
    if(const auto found=destination.extensions.find(name);found!=destination.extensions.end()) target.extensions[name]=found->second;
    else target.extensions.erase(name);
  }
  for(size_t p=0;p<owned.numeric.size();++p) if(owned.numeric[p]) {
    const auto property=P(p);
    if(!info(property).extension || !owned.wholeExtensions[size_t(*info(property).extension)]) copyProperty(target,destination,property);
  }
  for(size_t slot=0;slot<owned.textures.size();++slot) if(owned.textures[slot]) {
    const auto textureSlot=GltfMaterialTextureSlot(slot);
    const auto extension=ReadGltfMaterialTextureBinding(destination,textureSlot).extension;
    bool whole=false;for(size_t e=0;e<kExtensions.size();++e) if(extension==kExtensions[e]) whole=owned.wholeExtensions[e];
    if(!whole) CopyGltfMaterialTextureEditLeaves(target,destination,textureSlot,owned.textures[slot]);
  }
  if(owned.alphaMode) target.alphaMode=destination.alphaMode;
  if(owned.doubleSided) target.doubleSided=destination.doubleSided;
  if(owned.name) target.name=destination.name;
  if(exact(target.extensions,destination.extensions)) target.extensions_json_string=destination.extensions_json_string;
  else if(!exact(target.extensions,current.extensions)) target.extensions_json_string.clear();
}
} // namespace

std::span<const GltfMaterialPropertyInfo> GltfMaterialPropertyRegistry() noexcept {return kProperties;}
std::string_view GltfMaterialExtensionName(GltfMaterialExtension extension) {
  if(size_t(extension)>=kExtensions.size()) fail("unknown typed extension");return kExtensions[size_t(extension)];
}
GltfMaterialAuthoringPlan PlanGltfMaterialEdit(const tinygltf::Model& a,const tinygltf::Model& e,int32_t id,
    const GltfMaterialPatch& patch,const GltfMaterialAuthoringLimits& limits) {
  return plan(GltfMaterialSourceView(a),GltfMaterialSourceView(e),id,selected(a,id),selected(e,id),patch,GltfMaterialPlanKind::Edit,limits);
}
GltfMaterialAuthoringPlan PlanGltfMaterialDuplicate(const tinygltf::Model& a,const tinygltf::Model& e,int32_t id,
    const GltfMaterialPatch& patch,const GltfMaterialAuthoringLimits& limits) {
  return plan(GltfMaterialSourceView(a),GltfMaterialSourceView(e),id,selected(a,id),selected(e,id),patch,GltfMaterialPlanKind::Duplicate,limits);
}
GltfMaterialAuthoringPlan PlanGltfMaterialMakeUnique(const tinygltf::Model& a,const tinygltf::Model& e,int32_t id,
    const GltfMaterialPatch& patch,const GltfMaterialAuthoringLimits& limits) {
  return plan(GltfMaterialSourceView(a),GltfMaterialSourceView(e),id,selected(a,id),selected(e,id),patch,GltfMaterialPlanKind::MakeUnique,limits);
}
GltfMaterialAuthoringPlan PlanGltfMaterialAppend(const tinygltf::Model& a,const tinygltf::Model& e,
    const tinygltf::Material& authored,const tinygltf::Material& evaluated,const GltfMaterialPatch& patch,
    const GltfMaterialAuthoringLimits& limits) {
  return plan(GltfMaterialSourceView(a),GltfMaterialSourceView(e),{},authored,evaluated,patch,GltfMaterialPlanKind::Append,limits);
}
GltfMaterialAuthoringSourceView GltfMaterialSourceView(const tinygltf::Model& source) noexcept {
  return {source.materials.size(),source.textures.size(),source.extensionsUsed,source.animations};
}
void ValidateGltfMaterialRecord(const tinygltf::Material& material,GltfMaterialAuthoringSourceView source,
    const GltfMaterialAuthoringLimits& limits) {
  limitsValid(limits);
  if(source.extensionsUsed.size()>limits.maxMetadataValues) fail("declaration traversal exceeds its budget");
  Budget budget{limits};
  for(const auto& name:source.extensionsUsed) budget.add(sum(sizeof(name),name.size()));
  // Reserve all compact validation/probe scratch before the first record copy.
  for(size_t copy=0;copy<6;++copy) budget.material(material);
  validate(material,source,true);
}
GltfMaterialAuthoringPlan PlanGltfMaterialEdit(GltfMaterialAuthoringSourceView a,GltfMaterialAuthoringSourceView e,
    int32_t id,const tinygltf::Material& authored,const tinygltf::Material& evaluated,
    const GltfMaterialPatch& patch,const GltfMaterialAuthoringLimits& limits) {
  return plan(a,e,id,authored,evaluated,patch,GltfMaterialPlanKind::Edit,limits);
}
GltfMaterialAuthoringPlan PlanGltfMaterialDuplicate(GltfMaterialAuthoringSourceView a,GltfMaterialAuthoringSourceView e,
    int32_t id,const tinygltf::Material& authored,const tinygltf::Material& evaluated,
    const GltfMaterialPatch& patch,const GltfMaterialAuthoringLimits& limits) {
  return plan(a,e,id,authored,evaluated,patch,GltfMaterialPlanKind::Duplicate,limits);
}
GltfMaterialAuthoringPlan PlanGltfMaterialMakeUnique(GltfMaterialAuthoringSourceView a,GltfMaterialAuthoringSourceView e,
    int32_t id,const tinygltf::Material& authored,const tinygltf::Material& evaluated,
    const GltfMaterialPatch& patch,const GltfMaterialAuthoringLimits& limits) {
  return plan(a,e,id,authored,evaluated,patch,GltfMaterialPlanKind::MakeUnique,limits);
}
GltfMaterialAuthoringPlan PlanGltfMaterialAppend(GltfMaterialAuthoringSourceView a,GltfMaterialAuthoringSourceView e,
    const tinygltf::Material& authored,const tinygltf::Material& evaluated,
    const GltfMaterialPatch& patch,const GltfMaterialAuthoringLimits& limits) {
  return plan(a,e,{},authored,evaluated,patch,GltfMaterialPlanKind::Append,limits);
}
GltfMaterialAuthoringPlan PlanGltfMaterialTextureEdit(
    GltfMaterialAuthoringSourceView beforeAuthored,GltfMaterialAuthoringSourceView beforeEvaluated,
    GltfMaterialAuthoringSourceView afterAuthored,GltfMaterialAuthoringSourceView afterEvaluated,
    int32_t sourceMaterial,const tinygltf::Material& authoredMaterial,const tinygltf::Material& evaluatedMaterial,
    const GltfMaterialPatch& patch,const GltfMaterialAuthoringLimits& limits) {
  return plan(beforeAuthored,beforeEvaluated,sourceMaterial,authoredMaterial,evaluatedMaterial,patch,
      GltfMaterialPlanKind::Edit,limits,afterAuthored,afterEvaluated);
}
GltfMaterialAuthoringContextWitness GltfMaterialAuthoringContext(const GltfMaterialAuthoringPlan& plan) {
  const auto& receipt=proved(plan);
  return {plan.originalMaterialCount,receipt.authoredTextures,receipt.evaluatedTextures,
      receipt.authoredDeclarations,receipt.evaluatedDeclarations,receipt.afterAuthoredTextures,receipt.afterEvaluatedTextures,
      receipt.resourceTransition,
      receipt.resourceTransition?std::span<const std::string>(receipt.afterAuthoredDeclarations):std::span<const std::string>(receipt.authoredDeclarations),
      receipt.resourceTransition?std::span<const std::string>(receipt.afterEvaluatedDeclarations):std::span<const std::string>(receipt.evaluatedDeclarations)};
}
uint64_t GltfMaterialRecordStorageBytes(const tinygltf::Material& record,const GltfMaterialAuthoringLimits& limits) {
  limitsValid(limits);Budget budget{limits};budget.material(record);return budget.bytes;
}
bool GltfMaterialRecordsEqual(const tinygltf::Material& a,const tinygltf::Material& b) noexcept {
  return exact(a,b);
}
void ValidateGltfMaterialAuthoringPlan(const GltfMaterialAuthoringPlan& plan,GltfMaterialAuthoringSourceView authored,
    GltfMaterialAuthoringSourceView evaluated,const GltfMaterialAuthoringLimits& limits) {
  const auto& receipt=proved(plan);limitsValid(limits);
  if(receipt.resourceTransition) fail("resource-transition receipt requires independent Before/After validation");
  Budget budget{limits};chargePlan(budget,plan);chargePlan(budget,receipt.expected);chargePatch(budget,receipt.patch);
  chargeStrings(budget,receipt.authoredDeclarations);chargeStrings(budget,receipt.evaluatedDeclarations);
  if(receipt.command) {chargePlan(budget,receipt.command->expected);chargePatch(budget,receipt.command->patch);}
  validateContext(plan,receipt,authored,evaluated,budget);
}
void ValidateGltfMaterialEditBefore(const GltfMaterialAuthoringPlan& edit,
    const tinygltf::Material& currentAuthored,const tinygltf::Material& currentEvaluated,
    GltfMaterialAuthoringSourceView authored,GltfMaterialAuthoringSourceView evaluated,
    const GltfMaterialAuthoringLimits& limits) {
  ValidateGltfMaterialAuthoringPlan(edit,authored,evaluated,limits);
  if(edit.kind!=GltfMaterialPlanKind::Edit || !edit.sourceMaterial ||
      edit.destinationMaterial!=*edit.sourceMaterial ||
      !exact(currentAuthored,edit.authoredBefore) || !exact(currentEvaluated,edit.evaluatedBefore))
    fail("material edit no longer matches its immediate authored/evaluated before records");
}
void ValidateGltfMaterialEditTransition(const GltfMaterialAuthoringPlan& edit,
    const tinygltf::Material& currentAuthored,const tinygltf::Material& currentEvaluated,
    GltfMaterialAuthoringSourceView beforeAuthored,GltfMaterialAuthoringSourceView beforeEvaluated,
    GltfMaterialAuthoringSourceView afterAuthored,GltfMaterialAuthoringSourceView afterEvaluated,
    const GltfMaterialAuthoringLimits& limits) {
  const auto& receipt=proved(edit);limitsValid(limits);Budget budget{limits};
  validateIntrinsicTransition(edit,receipt,budget);
  validateContext(edit,receipt,beforeAuthored,beforeEvaluated,budget);
  validateContext(edit,receipt,afterAuthored,afterEvaluated,budget,true);
  if(!exact(currentAuthored,edit.authoredBefore) || !exact(currentEvaluated,edit.evaluatedBefore))
    fail("material resource transition no longer matches its immediate Before records");
  validate(currentAuthored,beforeAuthored,true);validate(currentEvaluated,beforeEvaluated,true);
  validate(edit.authoredAfter,afterAuthored,true);validate(edit.evaluatedAfter,afterEvaluated,true);
}
bool GltfMaterialAuthoringPlanMatches(const GltfMaterialAuthoringPlan& plan,GltfMaterialAuthoringSourceView authored,
    GltfMaterialAuthoringSourceView evaluated,const GltfMaterialAuthoringLimits& limits) noexcept {
  const auto& receipt=GltfMaterialAuthoringAccess::receipt(plan);
  if(!receipt || receipt->resourceTransition || !matchLimits(limits)) return false;
  MatchBudget budget{limits};
  if(!budget.plan(plan) || !budget.plan(receipt->expected) || !budget.patch(receipt->patch) ||
      !budget.strings(receipt->authoredDeclarations) || !budget.strings(receipt->evaluatedDeclarations) ||
      (receipt->command && (!budget.plan(receipt->command->expected) || !budget.patch(receipt->command->patch))) ||
      !exactPlan(plan,receipt->expected)) return false;
  return matchContext(plan,*receipt,authored,evaluated,budget);
}
bool GltfMaterialEditBeforeMatches(const GltfMaterialAuthoringPlan& plan,const tinygltf::Material& currentAuthored,
    const tinygltf::Material& currentEvaluated,GltfMaterialAuthoringSourceView authored,
    GltfMaterialAuthoringSourceView evaluated,const GltfMaterialAuthoringLimits& limits) noexcept {
  return GltfMaterialAuthoringPlanMatches(plan,authored,evaluated,limits) && plan.kind==GltfMaterialPlanKind::Edit &&
      plan.sourceMaterial && plan.destinationMaterial==*plan.sourceMaterial && matchBefore(plan,currentAuthored,currentEvaluated,limits);
}
bool GltfMaterialEditTransitionMatches(const GltfMaterialAuthoringPlan& plan,const tinygltf::Material& currentAuthored,
    const tinygltf::Material& currentEvaluated,GltfMaterialAuthoringSourceView beforeAuthored,
    GltfMaterialAuthoringSourceView beforeEvaluated,GltfMaterialAuthoringSourceView afterAuthored,
    GltfMaterialAuthoringSourceView afterEvaluated,const GltfMaterialAuthoringLimits& limits) noexcept {
  const auto& receipt=GltfMaterialAuthoringAccess::receipt(plan);
  if(!receipt || !receipt->resourceTransition || !matchLimits(limits) || plan.kind!=GltfMaterialPlanKind::Edit ||
      !plan.sourceMaterial || plan.destinationMaterial!=*plan.sourceMaterial) return false;
  MatchBudget budget{limits};
  if(!budget.plan(plan) || !budget.receipt(*receipt) || (receipt->command && !budget.receipt(*receipt->command)) ||
      !exactPlan(plan,receipt->expected) || !matchBefore(plan,currentAuthored,currentEvaluated,limits)) return false;
  // These exact private Before/After records were semantically validated when
  // minted. Captured resource counts/declarations and current raw bindings must
  // still match; publication does not rerun the allocating material converter.
  return matchContext(plan,*receipt,beforeAuthored,beforeEvaluated,budget) &&
      matchContext(plan,*receipt,afterAuthored,afterEvaluated,budget,true);
}
GltfMaterialAuthoringPlan CoalesceGltfMaterialEdit(const GltfMaterialAuthoringPlan& cumulative,
    const GltfMaterialAuthoringPlan& next,GltfMaterialAuthoringSourceView authored,
    GltfMaterialAuthoringSourceView evaluated,const GltfMaterialAuthoringLimits& limits) {
  // Prove public records before inspecting their identities or reserving any
  // output. These comparisons allocate nothing and preserve exact raw bits.
  const auto& first=proved(cumulative);const auto& last=proved(next);limitsValid(limits);
  if(first.command || last.command || cumulative.kind!=GltfMaterialPlanKind::Edit || next.kind!=GltfMaterialPlanKind::Edit ||
      first.resourceTransition || last.resourceTransition ||
      !cumulative.sourceMaterial || cumulative.sourceMaterial!=next.sourceMaterial ||
      cumulative.destinationMaterial!=*cumulative.sourceMaterial || next.destinationMaterial!=cumulative.destinationMaterial ||
      cumulative.originalMaterialCount!=next.originalMaterialCount || !exactBindings(cumulative.animationBindings,next.animationBindings))
    fail("only compatible direct material edit commands can be coalesced");
  if(!exact(cumulative.authoredAfter,next.authoredBefore)) fail("material preview authored chain does not join exactly");
  GltfMaterialEditOwnership owned=first.ownership;
  for(size_t p=0;p<owned.numeric.size();++p) owned.numeric[p]=owned.numeric[p] || last.ownership.numeric[p];
  for(size_t e=0;e<owned.wholeExtensions.size();++e) owned.wholeExtensions[e]=owned.wholeExtensions[e] || last.ownership.wholeExtensions[e];
  for(size_t slot=0;slot<owned.textures.size();++slot) owned.textures[slot]|=last.ownership.textures[slot];
  owned.alphaMode|=last.ownership.alphaMode;owned.doubleSided|=last.ownership.doubleSided;owned.name|=last.ownership.name;
  const auto operations=uint64_t(std::ranges::count(owned.numeric,true))+uint64_t(std::ranges::count(owned.wholeExtensions,true))+
      uint64_t(std::ranges::count_if(owned.textures,[](auto mask) {return mask!=0;}))+
      uint64_t(owned.alphaMode)+uint64_t(owned.doubleSided)+uint64_t(owned.name);
  if(operations>limits.maxPatchOperations) fail("cumulative material ownership exceeds its operation budget");

  Budget budget{limits};
  // Both public inputs and their proofs coexist with replacement and scratch.
  // Charge actual retained records, never their historical preparation peaks;
  // repeated preview composition therefore has fixed, bounded retained cost.
  for(const auto* input:{&cumulative,&next}) {
    const auto& proof=proved(*input);
    budget.add(sizeof(GltfMaterialAuthoringPlan)+sizeof(GltfMaterialAuthoringReceipt));
    chargePlan(budget,*input);chargePlan(budget,proof.expected);chargePatch(budget,proof.patch);
    chargeStrings(budget,proof.authoredDeclarations);chargeStrings(budget,proof.evaluatedDeclarations);
  }
  // Output/proof copies and the compact validation/probe witnesses are reserved
  // BEFORE either context's binding scratch or a material copy is allocated.
  for(size_t copy=0;copy<6;++copy) {
    budget.material(cumulative.authoredBefore);budget.material(cumulative.evaluatedBefore);
    budget.material(next.authoredAfter);budget.material(next.evaluatedAfter);
  }
  budget.add(2*(size_t(P::Count)*sizeof(P)+size_t(E::Count)*sizeof(E)+size_t(GltfMaterialTextureSlot::Count)*sizeof(GltfMaterialTextureSlot)));
  for(const auto& binding:cumulative.animationBindings) {
    const auto bytes=sum(sizeof(binding),binding.path.size());budget.add(sum(bytes,bytes));
  }
  validateContext(cumulative,first,authored,evaluated,budget);validateContext(next,last,authored,evaluated,budget);
  validate(cumulative.authoredBefore,authored,true);validate(cumulative.evaluatedBefore,evaluated,true);
  validate(next.authoredAfter,authored,true);validate(next.evaluatedAfter,evaluated,true);

  GltfMaterialAuthoringPlan result;result.kind=GltfMaterialPlanKind::Edit;result.sourceMaterial=cumulative.sourceMaterial;
  result.destinationMaterial=cumulative.destinationMaterial;result.originalMaterialCount=cumulative.originalMaterialCount;
  result.authoredBefore=cumulative.authoredBefore;result.evaluatedBefore=cumulative.evaluatedBefore;
  result.authoredAfter=next.authoredAfter;result.evaluatedAfter=next.evaluatedAfter;
  for(size_t p=0;p<owned.numeric.size();++p) if(owned.numeric[p]) result.touchedProperties.push_back(P(p));
  for(size_t slot=0;slot<owned.textures.size();++slot) if(owned.textures[slot]) result.touchedTextures.push_back(GltfMaterialTextureSlot(slot));
  for(size_t e=0;e<size_t(E::Count);++e)
    if(std::ranges::find(cumulative.touchedExtensions,E(e))!=cumulative.touchedExtensions.end() ||
       std::ranges::find(next.touchedExtensions,E(e))!=next.touchedExtensions.end()) result.touchedExtensions.push_back(E(e));
  result.animationBindings=cumulative.animationBindings;
  declarations(result,authored,evaluated,budget);
  result.changed=!exact(result.authoredBefore,result.authoredAfter) || !exact(result.evaluatedBefore,result.evaluatedAfter);
  // The fixed ownership mask replaces an executable final patch: a legitimate
  // sequence can both edit and later disable one extension. Retain no input
  // receipts, earlier intermediate records, or frame-by-frame command chain.
  mint(result,{},authored,evaluated,budget,owned);
  return result;
}
GltfMaterialAuthoringPlan RebaseGltfMaterialEdit(const GltfMaterialAuthoringPlan& original,GltfMaterialEditSide side,
    const tinygltf::Material& currentAuthored,const tinygltf::Material& currentEvaluated,
    GltfMaterialAuthoringSourceView authored,GltfMaterialAuthoringSourceView evaluated,const GltfMaterialAuthoringLimits& limits) {
  ValidateGltfMaterialAuthoringPlan(original,authored,evaluated,limits);
  const auto& proof=GltfMaterialAuthoringAccess::receipt(original);
  const auto command=proof->command?proof->command:proof;
  const auto& trusted=command->expected;
  if(trusted.kind!=GltfMaterialPlanKind::Edit) fail("only an existing-material edit can be rebased");
  if(side!=GltfMaterialEditSide::Before && side!=GltfMaterialEditSide::After) fail("unknown material rebase side");
  const bool before=side==GltfMaterialEditSide::Before;
  const auto& expectedAuthored=before?trusted.authoredAfter:trusted.authoredBefore;
  const auto& destinationAuthored=before?trusted.authoredBefore:trusted.authoredAfter;
  const auto& destinationEvaluated=before?trusted.evaluatedBefore:trusted.evaluatedAfter;
  if(!exact(currentAuthored,expectedAuthored)) fail("current authored material is not the opposite command side");
  Budget budget{limits};
  // Includes public+proved output, validation/probe scratch, and the retained
  // original command receipt. No current-clock material is copied beforehand.
  for(size_t copy=0;copy<6;++copy) {budget.material(currentAuthored);budget.material(currentEvaluated);}
  // The inverse may restore a large extension absent from CURRENT material.
  // Reserve the trusted destination independently; a current-only estimate
  // would miss its public/proof/validation copies during undo.
  for(size_t copy=0;copy<6;++copy) {budget.material(destinationAuthored);budget.material(destinationEvaluated);}
  chargePlan(budget,trusted);chargePatch(budget,command->patch);
  chargeStrings(budget,command->authoredDeclarations);chargeStrings(budget,command->evaluatedDeclarations);
  budget.add(sum(sum(command->patch.values.size(),command->patch.extensions.size()),command->patch.textures.size())*8192);
  if(command->patch.name) budget.add(command->patch.name->size()*8);
  validate(currentAuthored,authored,true);validate(currentEvaluated,evaluated,true);
  GltfMaterialAuthoringPlan result;result.kind=trusted.kind;result.sourceMaterial=trusted.sourceMaterial;
  result.destinationMaterial=trusted.destinationMaterial;result.originalMaterialCount=trusted.originalMaterialCount;
  result.authoredBefore=currentAuthored;result.authoredAfter=destinationAuthored;
  result.evaluatedBefore=currentEvaluated;result.evaluatedAfter=currentEvaluated;
  const auto& owned=command->ownership;
  copyOwnedEvaluated(result.evaluatedAfter,currentEvaluated,destinationEvaluated,owned);
  validate(result.authoredAfter,authored,true);validate(result.evaluatedAfter,evaluated,true);
  result.touchedProperties=trusted.touchedProperties;result.touchedExtensions=trusted.touchedExtensions;
  result.touchedTextures=trusted.touchedTextures;
  result.animationBindings=trusted.animationBindings;
  declarations(result,authored,evaluated,budget);
  result.changed=!exact(result.authoredBefore,result.authoredAfter) || !exact(result.evaluatedBefore,result.evaluatedAfter);
  mint(result,command->patch,authored,evaluated,budget,owned,command);
  return result;
}
GltfMaterialAuthoringPlan RebaseGltfMaterialTextureEdit(const GltfMaterialAuthoringPlan& original,GltfMaterialEditSide side,
    const tinygltf::Material& currentAuthored,const tinygltf::Material& currentEvaluated,
    GltfMaterialAuthoringSourceView beforeAuthored,GltfMaterialAuthoringSourceView beforeEvaluated,
    GltfMaterialAuthoringSourceView afterAuthored,GltfMaterialAuthoringSourceView afterEvaluated,
    const GltfMaterialAuthoringLimits& limits) {
  const auto& proof=proved(original);limitsValid(limits);Budget budget{limits};
  validateIntrinsicTransition(original,proof,budget);
  const auto command=proof.command?proof.command:GltfMaterialAuthoringAccess::receipt(original);
  const auto& trusted=command->expected;
  if(!command->resourceTransition || trusted.kind!=GltfMaterialPlanKind::Edit ||
      (side!=GltfMaterialEditSide::Before && side!=GltfMaterialEditSide::After)) fail("invalid texture resource replay command or side");
  const bool undo=side==GltfMaterialEditSide::Before;
  const auto& expectedAuthored=undo?trusted.authoredAfter:trusted.authoredBefore;
  const auto& destinationAuthored=undo?trusted.authoredBefore:trusted.authoredAfter;
  const auto& destinationEvaluated=undo?trusted.evaluatedBefore:trusted.evaluatedAfter;
  if(!exact(currentAuthored,expectedAuthored)) fail("current authored material is not the opposite texture command side");
  if(beforeAuthored.materialCount!=trusted.originalMaterialCount || beforeEvaluated.materialCount!=trusted.originalMaterialCount ||
      afterAuthored.materialCount!=trusted.originalMaterialCount || afterEvaluated.materialCount!=trusted.originalMaterialCount ||
      beforeAuthored.textureCount>INT_MAX || beforeEvaluated.textureCount>INT_MAX ||
      afterAuthored.textureCount>INT_MAX || afterEvaluated.textureCount>INT_MAX) fail("texture replay resource/material domains are invalid");
  if(beforeAuthored.extensionsUsed.size()>limits.maxMetadataValues || beforeEvaluated.extensionsUsed.size()>limits.maxMetadataValues ||
      afterAuthored.extensionsUsed.size()>limits.maxMetadataValues || afterEvaluated.extensionsUsed.size()>limits.maxMetadataValues)
    fail("texture replay declaration traversal exceeds its budget");
  // The outer resource command proves prefix/source cookies. Here current and
  // prospective records are independently bounded in their actual domains,
  // rather than widening either side to max(Before,After).
  for(size_t copy=0;copy<6;++copy) {
    budget.material(currentAuthored);budget.material(currentEvaluated);
    budget.material(destinationAuthored);budget.material(destinationEvaluated);
  }
  budget.add(sum(sum(command->patch.values.size(),command->patch.extensions.size()),command->patch.textures.size())*8192);
  validate(currentAuthored,beforeAuthored,true);validate(currentEvaluated,beforeEvaluated,true);
  auto tracked=bindings(beforeAuthored,*trusted.sourceMaterial,budget);
  if(!exactBindings(tracked,trusted.animationBindings) || !exactBindings(bindings(beforeEvaluated,*trusted.sourceMaterial,budget),trusted.animationBindings) ||
      !exactBindings(bindings(afterAuthored,*trusted.sourceMaterial,budget),trusted.animationBindings) ||
      !exactBindings(bindings(afterEvaluated,*trusted.sourceMaterial,budget),trusted.animationBindings)) fail("texture replay animation target context changed");
  GltfMaterialAuthoringPlan result;result.kind=GltfMaterialPlanKind::Edit;result.sourceMaterial=trusted.sourceMaterial;
  result.destinationMaterial=trusted.destinationMaterial;result.originalMaterialCount=trusted.originalMaterialCount;
  result.authoredBefore=currentAuthored;result.authoredAfter=destinationAuthored;
  result.evaluatedBefore=currentEvaluated;result.evaluatedAfter=currentEvaluated;
  copyOwnedEvaluated(result.evaluatedAfter,currentEvaluated,destinationEvaluated,command->ownership);
  validate(result.authoredAfter,afterAuthored,true);validate(result.evaluatedAfter,afterEvaluated,true);
  result.touchedProperties=trusted.touchedProperties;result.touchedExtensions=trusted.touchedExtensions;result.touchedTextures=trusted.touchedTextures;
  result.animationBindings=trusted.animationBindings;declarations(result,afterAuthored,afterEvaluated,budget);
  result.changed=!exact(result.authoredBefore,result.authoredAfter) || !exact(result.evaluatedBefore,result.evaluatedAfter);
  mint(result,command->patch,beforeAuthored,beforeEvaluated,budget,command->ownership,command,afterAuthored,afterEvaluated);
  return result;
}
} // namespace container::geometry::gltf
