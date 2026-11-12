#include <Container/geometry/GltfMerge.h>
#include <Container/geometry/GltfMergePlacement.h>
#include <Container/geometry/GltfAccessor.h>
#include <Container/geometry/GltfAnimation.h>
#include <Container/geometry/GltfCapabilities.h>
#include <Container/geometry/GltfInteractivity.h>
#include <Container/geometry/GltfIesProfiles.h>
#include <Container/geometry/GltfMaterialVariants.h>
#include <Container/geometry/GltfSceneCatalog.h>
#include <Container/geometry/GltfXmpMetadata.h>
#include <Container/utility/ModelImportCancellation.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <climits>
#include <limits>
#include <set>
#include <span>
#include <stdexcept>
#include <string_view>

namespace container::geometry::gltf {
namespace {
using container::util::checkModelImportCancellation;
using Value=tinygltf::Value;
using Object=Value::Object;
using Array=Value::Array;
using Names=std::initializer_list<std::string_view>;
constexpr const char* kVariants="KHR_materials_variants";
constexpr const char* kPunctual="KHR_lights_punctual";
constexpr const char* kPointer="KHR_animation_pointer";
constexpr const char* kInstancing="EXT_mesh_gpu_instancing";
constexpr std::array kIndexedLights{"KHR_lights_area","EXT_lights_area","WEBGI_lights_area","WEBGI_area_light",kGltfIesExtension};

[[noreturn]] void fail(std::string_view context,std::string_view reason) {
  throw std::runtime_error("glTF merge: "+std::string(context)+": "+std::string(reason));
}
uint64_t add(uint64_t a,uint64_t b,std::string_view where) {
  if(b>UINT64_MAX-a) fail(where,"integer overflow"); return a+b;
}
uint64_t mul(uint64_t a,uint64_t b,std::string_view where) {
  if(a && b>UINT64_MAX/a) fail(where,"integer overflow"); return a*b;
}
void charge(uint64_t& total,uint64_t count,uint64_t limit,std::string_view where) {
  total=add(total,count,where); if(total>limit) fail(where,"aggregate limit exceeded");
}
int index(int id,size_t count,std::string_view where,bool optional=false) {
  if(optional && id==-1) return id;
  if(id<0 || size_t(id)>=count) fail(where,"index is outside its source table"); return id;
}
int integer(const Value& value,size_t count,std::string_view where) {
  if(!value.IsInt()) fail(where,"reference must be an integer");
  return index(value.Get<int>(),count,where);
}
const Object& object(const Value& value,std::string_view where) {
  if(!value.IsObject()) fail(where,"expected an object"); return value.Get<Object>();
}
const Array& array(const Value& value,std::string_view where) {
  if(!value.IsArray()) fail(where,"expected an array"); return value.Get<Array>();
}
const Value& field(const Value& value,const char* key,std::string_view where) {
  const auto& values=object(value,where); const auto found=values.find(key);
  if(found==values.end()) fail(where,std::string("missing ")+key); return found->second;
}
bool named(std::string_view name,Names names) {return std::ranges::find(names,name)!=names.end();}
bool indexedLight(std::string_view name) {return std::ranges::find(kIndexedLights,name)!=kIndexedLights.end();}
void finite(double value,std::string_view where) {
  if(!std::isfinite(value)) fail(where,"nonfinite numeric value");
}
void finite(std::span<const double> values,std::string_view where) {
  for(const auto value:values) finite(value,where);
}

struct Budget {
  const GltfMergeLimits& limits;
  const std::stop_token& stop;
  GltfMergeSummary sum;
  std::set<std::string_view> opaqueDestinationExtensions;
  void text(std::string_view value) {charge(sum.metadataBytes,add(value.size(),8,"metadata"),limits.maxMetadataBytes,"metadata");}
  void bytes(uint64_t n) {charge(sum.metadataBytes,n,limits.maxMetadataBytes,"metadata");}
  void refs(uint64_t n) {charge(sum.references,n,limits.maxReferences,"references"); bytes(mul(n,16,"references"));}
  void objects(uint64_t n) {charge(sum.objects,n,limits.maxTotalObjects,"objects"); bytes(mul(n,64,"objects"));}
  void value(const Value& value,uint32_t depth=0) {
    checkModelImportCancellation(stop);
    if(depth>limits.maxMetadataDepth) fail("metadata","nesting limit exceeded");
    bytes(16);
    if(value.IsString()) text(value.Get<std::string>());
    else if(value.IsNumber()) finite(value.GetNumberAsDouble(),"metadata");
    else if(value.IsArray()) for(const auto& child:value.Get<Array>()) this->value(child,depth+1);
    else if(value.IsObject()) for(const auto& [key,child]:value.Get<Object>()) {text(key);this->value(child,depth+1);}
    else if(value.IsBinary()) bytes(value.Get<std::vector<unsigned char>>().size());
  }
};

// extras are opaque; nested extension maps still require known schema handlers.
void nested(const Value& value,std::string_view label,const Budget& budget,
            bool textureTransforms=false,bool lightAreas=false) {
  if(value.IsObject()) for(const auto& [key,child]:value.Get<Object>()) {
    if(key=="extras") continue;
    if(key=="extensions") {
      for(const auto& [name,extension]:object(child,label)) {
        if(budget.opaqueDestinationExtensions.contains(name)) {object(extension,label);continue;}
        if(!(textureTransforms && name=="KHR_texture_transform") && !(lightAreas && indexedLight(name)))
          fail(label,"unsupported nested extension reference: "+name);
        object(extension,label); nested(extension,label,budget,false);
      }
    } else nested(child,label,budget,textureTransforms,lightAreas);
  } else if(value.IsArray()) for(const auto& child:value.Get<Array>()) nested(child,label,budget,textureTransforms,lightAreas);
}
bool declared(const tinygltf::Model& model,std::string_view name) {
  return std::ranges::find(model.extensionsUsed,name)!=model.extensionsUsed.end();
}
void extensions(const tinygltf::ExtensionMap& values,const tinygltf::Model& model,
    Budget& budget,std::string_view label,Names allowed={},bool transforms=false) {
  for(const auto& [name,value]:values) {
    const bool opaque=budget.opaqueDestinationExtensions.contains(name);
    if(!opaque && !named(name,allowed)) fail(label,"unsupported extension reference: "+name);
    if(!declared(model,name)) fail(label,"extension missing from extensionsUsed: "+name);
    budget.text(name);budget.value(value);
    // XMP's raw packet package and attachment diagnostics live outside
    // TinyGLTF's typed extension validation.
    if(name==kGltfXmpExtension) continue;
    object(value,label);
    if(!opaque) nested(value,label,budget,transforms,label=="root" && (name==kPunctual || indexedLight(name)));
  }
}
template<class T> void property(const T& value,const tinygltf::Model& model,
    Budget& budget,std::string_view label,Names allowed={},bool transforms=false) {
  budget.objects(1);
  if constexpr(requires {value.name;}) budget.text(value.name);
  budget.value(value.extras);
  budget.text(value.extras_json_string);budget.text(value.extensions_json_string);
  extensions(value.extensions,model,budget,label,allowed,transforms);
}
template<class T> void textureInfo(const T& info,const tinygltf::Model& model,Budget& budget) {
  property(info,model,budget,"texture info",{"KHR_texture_transform"});
  index(info.index,model.textures.size(),"texture info",true);
  if(info.texCoord<0) fail("texture info","negative UV selector");
  int effective=info.texCoord;
  if(const auto ext=info.extensions.find("KHR_texture_transform");ext!=info.extensions.end())
    if(ext->second.Has("texCoord")) effective=integer(ext->second.Get("texCoord"),2,"texture transform UV set");
  if(effective>1) fail("texture info","only effective UV0/UV1 are supported");
  budget.refs(info.index>=0);
}

constexpr std::array kMaterialTextureFields{
  std::pair{"KHR_materials_specular","specularTexture"},
  std::pair{"KHR_materials_specular","specularColorTexture"},
  std::pair{"KHR_materials_transmission","transmissionTexture"},
  std::pair{"KHR_materials_volume","thicknessTexture"},
  std::pair{"KHR_materials_clearcoat","clearcoatTexture"},
  std::pair{"KHR_materials_clearcoat","clearcoatRoughnessTexture"},
  std::pair{"KHR_materials_clearcoat","clearcoatNormalTexture"},
  std::pair{"KHR_materials_sheen","sheenColorTexture"},
  std::pair{"KHR_materials_sheen","sheenRoughnessTexture"},
  std::pair{"KHR_materials_iridescence","iridescenceTexture"},
  std::pair{"KHR_materials_iridescence","iridescenceThicknessTexture"},
  std::pair{"KHR_materials_anisotropy","anisotropyTexture"},
  std::pair{"KHR_materials_diffuse_transmission","diffuseTransmissionTexture"},
  std::pair{"KHR_materials_diffuse_transmission","diffuseTransmissionColorTexture"},
  std::pair{"KHR_materials_retroreflection","retroreflectionTexture"},
  std::pair{"KHR_materials_scatter","scatterStrengthTexture"},
  std::pair{"KHR_materials_scatter","multiscatterColorTexture"},
  std::pair{"KHR_materials_volume_scatter","scatterStrengthTexture"},
  std::pair{"KHR_materials_volume_scatter","multiscatterColorTexture"},
  std::pair{"KHR_materials_pbrSpecularGlossiness","diffuseTexture"},
  std::pair{"KHR_materials_pbrSpecularGlossiness","specularGlossinessTexture"}};

void extensionTexture(const Value& info,const tinygltf::Model& model,Budget& budget) {
  integer(field(info,"index","extension texture"),model.textures.size(),"extension texture");
  int effective=0;
  if(info.Has("texCoord")) {
    const auto& uv=info.Get("texCoord");if(!uv.IsInt() || uv.Get<int>()<0) fail("extension texture","invalid UV selector");effective=uv.Get<int>();
  }
  if(info.Has("extensions")) {
    const auto& ext=object(info.Get("extensions"),"extension texture");
    if(const auto transform=ext.find("KHR_texture_transform");transform!=ext.end())
      if(transform->second.Has("texCoord")) effective=integer(transform->second.Get("texCoord"),2,"extension texture UV override");
  }
  if(effective>1) fail("extension texture","only effective UV0/UV1 are supported");
  budget.refs(1);
}

bool legacyTexture(std::string_view key) {
  return named(key,{"baseColorTexture","metallicRoughnessTexture","normalTexture","occlusionTexture","emissiveTexture",
    "roughnessTexture","metalnessTexture","specularTexture","heightTexture","displacementTexture","opacityTexture","refractionTexture","transmissionTexture"});
}

struct Audit {
  container::scene::GltfSceneCatalog catalog;
  std::vector<int> parents;
  size_t variants{};
  std::map<std::string,size_t> extensionLightCounts;
};

uint64_t channelDecodedBytes(const tinygltf::Model& model,const tinygltf::Animation& animation,
    const tinygltf::AnimationChannel& channel) {
  const auto& sampler=animation.samplers[size_t(index(channel.sampler,animation.samplers.size(),"animation sampler"))];
  uint64_t bytes=0;
  for(const auto id:{sampler.input,sampler.output}) {
    const auto& accessor=model.accessors[size_t(index(id,model.accessors.size(),"animation accessor"))];
    const auto components=tinygltf::GetNumComponentsInType(uint32_t(accessor.type));
    if(components<=0) fail("animation accessor","invalid type");
    bytes=add(bytes,mul(mul(accessor.count,uint64_t(components),"animation expansion"),sizeof(double),"animation expansion"),"animation expansion");
  }
  return bytes;
}

void tableCount(size_t count,const GltfMergeLimits& limits,std::string_view label) {
  if(count>limits.maxTableEntries || count>INT_MAX) fail(label,"table limit exceeded");
}
bool supportedDeclaration(std::string_view name) {
  return indexedLight(name) || named(name,{kVariants,kPunctual,kPointer,kInstancing,kGltfXmpExtension,
    "KHR_texture_transform","KHR_node_visibility","KHR_node_selectability","KHR_node_hoverability",kGltfInteractivityExtension,
    "KHR_texture_basisu","EXT_texture_webp","MSFT_texture_dds","KHR_mesh_quantization","KHR_accessor_float64",
    "KHR_materials_unlit","KHR_materials_specular","KHR_materials_ior","KHR_materials_dispersion",
    "KHR_materials_emissive_strength","KHR_materials_transmission","KHR_materials_volume",
    "KHR_materials_clearcoat","KHR_materials_sheen","KHR_materials_iridescence",
    "KHR_materials_anisotropy","KHR_materials_diffuse_transmission","KHR_materials_pbrSpecularGlossiness",
    "KHR_materials_retroreflection","KHR_materials_scatter","KHR_materials_volume_scatter"});
}

Audit audit(const tinygltf::Model& model,Budget& b,std::string_view label,bool preserveOpaque=false) {
  const auto& lim=b.limits;
  for(const auto count:{model.buffers.size(),model.bufferViews.size(),model.accessors.size(),model.images.size(),
      model.samplers.size(),model.textures.size(),model.materials.size(),model.meshes.size(),model.nodes.size(),
      model.skins.size(),model.cameras.size(),model.lights.size(),model.animations.size(),model.scenes.size()})
    tableCount(count,lim,label);
  if(model.nodes.size()>lim.maxNodes || model.scenes.size()>lim.maxScenes) fail(label,"node/scene limit exceeded");
  if(!model.audioEmitters.empty() || !model.audioSources.empty()) fail(label,"audio references need a merge handler");
  const auto version = validateCapabilities({model.asset.version,
      model.asset.minVersion.empty() ? std::nullopt : std::optional{model.asset.minVersion}}, label);
  for (const auto& diagnostic : version.diagnostics)
    if (diagnostic.severity == CapabilitySeverity::Error) fail(label, diagnostic.message);
  std::set<std::string> used,required;
  for(const auto& name:model.extensionsUsed) {
    b.text(name);
    if(!used.insert(name).second) fail(label,"duplicate declaration: "+name);
    if(!supportedDeclaration(name)) {
      // Destination indices stay stable. Preserve bounded optional metadata
      // that the runtime ignores without interpreting or remapping its values.
      const auto* capability=extensionCapability(name);
      if(!preserveOpaque || (capability && capability->support!=ExtensionSupport::Unsupported) ||
          std::ranges::find(model.extensionsRequired,name)!=model.extensionsRequired.end())
        fail(label,"unsupported declaration: "+name);
      b.opaqueDestinationExtensions.insert(name);
    }
  }
  for(const auto& name:model.extensionsRequired) {
    b.text(name);
    if(!required.insert(name).second || !used.contains(name)) fail(label,"invalid required extension declaration");
  }
  property(model.asset,model,b,"asset",{kGltfXmpExtension});
  b.text(model.asset.version);b.text(model.asset.minVersion);b.text(model.asset.generator);b.text(model.asset.copyright);
  b.value(model.extras);b.text(model.extras_json_string);b.text(model.extensions_json_string);
  extensions(model.extensions,model,b,"root",{kVariants,kPunctual,kGltfInteractivityExtension,kGltfIesExtension,kGltfXmpExtension,
      "KHR_lights_area","EXT_lights_area","WEBGI_lights_area","WEBGI_area_light"});
  ValidateGltfInteractivity(model);
  Audit out;
  uint64_t edges=0;
  for(const auto& node:model.nodes) charge(edges,node.children.size(),lim.maxReferences,"hierarchy edges");
  if(add(b.sum.references,edges,"hierarchy edges")>lim.maxReferences) fail("hierarchy edges","aggregate limit exceeded");
  out.catalog=container::scene::BuildGltfSceneCatalog(
      model,{lim.maxScenes,lim.maxRootReferences-b.sum.rootReferences},b.stop);
  out.parents.assign(model.nodes.size(),-1);
  for(size_t i=0;i<model.nodes.size();++i) for(const auto child:model.nodes[i].children) out.parents[size_t(child)]=int(i);
  for(const auto& scene:out.catalog.entries) {
    charge(b.sum.rootReferences,scene.sourceRoots.size(),lim.maxRootReferences,"scene roots");b.refs(scene.sourceRoots.size());
  }
  for(const auto& scene:model.scenes) {
    property(scene,model,b,"scene",{kGltfXmpExtension}); if(!scene.audioEmitters.empty()) fail("scene","audio references need a merge handler");
  }
  if(model.scenes.empty()) {b.objects(1);b.text("Scene");}
  if(const auto root=model.extensions.find(kVariants);root!=model.extensions.end()) {
    out.variants=array(field(root->second,"variants",kVariants),kVariants).size();tableCount(out.variants,lim,kVariants);
  }
  for(const auto name:kIndexedLights) if(const auto ext=model.extensions.find(name);ext!=model.extensions.end()) {
    const auto count=array(field(ext->second,"lights",name),name).size();
    out.extensionLightCounts[name]=count;tableCount(count,lim,name);
    if (name==kGltfIesExtension) for(size_t i=0;i<count;++i)
      if (!GltfIesEmbeddedProfileUri(model,i)) fail(name,"profile was not embedded during source load");
  }
  if(const auto ext=model.extensions.find(kPunctual);ext!=model.extensions.end()) {
    if(array(field(ext->second,"lights",kPunctual),kPunctual).size()!=model.lights.size())
      fail(kPunctual,"typed and root light tables differ in size");
  }
  for(const auto& buffer:model.buffers) {
    property(buffer,model,b,"buffer");b.text(buffer.uri);
    charge(b.sum.payloadBytes,buffer.data.size(),lim.maxResidentBytes,"resident buffers");
  }
  for(const auto& view:model.bufferViews) {
    property(view,model,b,"buffer view");index(view.buffer,model.buffers.size(),"buffer view");b.refs(1);
    const auto size=model.buffers[size_t(view.buffer)].data.size();
    if(view.byteOffset>size || view.byteLength>size-view.byteOffset) fail("buffer view","range exceeds resident buffer");
  }
  for(size_t i=0;i<model.accessors.size();++i) {
    const auto& a=model.accessors[i];property(a,model,b,"accessor");
    finite(a.minValues,"accessor min");finite(a.maxValues,"accessor max");b.refs(a.minValues.size()+a.maxValues.size());
    const int components=tinygltf::GetNumComponentsInType(uint32_t(a.type));
    if(components<=0) fail("accessor","invalid type");
    charge(b.sum.accessorDecodedBytes,mul(mul(a.count,uint64_t(components),"accessor"),sizeof(double),"accessor"),lim.maxAccessorDecodedBytes,"expanded accessors");
    if(a.bufferView>=0) {index(a.bufferView,model.bufferViews.size(),"accessor");b.refs(1);}
    if(a.sparse.isSparse) {
      property(a.sparse,model,b,"sparse accessor");property(a.sparse.indices,model,b,"sparse indices");property(a.sparse.values,model,b,"sparse values");
      index(a.sparse.indices.bufferView,model.bufferViews.size(),"sparse indices");
      index(a.sparse.values.bufferView,model.bufferViews.size(),"sparse values");b.refs(2);
    }
    (void)CheckedAccessor(model,int(i),"merge accessor",
        {AccessorUsage::Generic,HasRequiredExtension(model,"KHR_accessor_float64")},b.stop);
  }
  for(const auto& image:model.images) {
    property(image,model,b,"image",{kGltfXmpExtension});b.text(image.uri);b.text(image.mimeType);
    index(image.bufferView,model.bufferViews.size(),"image",true);b.refs(image.bufferView>=0);
    if(image.bufferView<0 && image.image.empty()) fail("image","image payload is not resident");
    charge(b.sum.payloadBytes,image.image.size(),lim.maxResidentBytes,"resident images");
  }
  for(const auto& sampler:model.samplers) property(sampler,model,b,"sampler");
  for(const auto& texture:model.textures) {
    property(texture,model,b,"texture",{"KHR_texture_basisu","EXT_texture_webp","MSFT_texture_dds"});
    index(texture.source,model.images.size(),"texture image",true);index(texture.sampler,model.samplers.size(),"texture sampler",true);b.refs(2);
    bool source=texture.source>=0;
    for(const auto& [name,value]:texture.extensions) {
      if(b.opaqueDestinationExtensions.contains(name)) continue;
      integer(field(value,"source",name),model.images.size(),name);b.refs(1);source=true;
    }
    if(!source) fail("texture","missing image source");
  }
  for(const auto& material:model.materials) {
    property(material,model,b,"material",{"KHR_materials_unlit","KHR_materials_specular","KHR_materials_ior","KHR_materials_dispersion",
      "KHR_materials_emissive_strength","KHR_materials_transmission","KHR_materials_volume","KHR_materials_clearcoat","KHR_materials_sheen",
      "KHR_materials_iridescence","KHR_materials_anisotropy","KHR_materials_diffuse_transmission","KHR_materials_pbrSpecularGlossiness",
      "KHR_materials_retroreflection","KHR_materials_scatter","KHR_materials_volume_scatter",kGltfXmpExtension},true);
    if(!material.lods.empty()) fail("material","LOD references need a merge handler");
    property(material.pbrMetallicRoughness,model,b,"metallic roughness");
    textureInfo(material.pbrMetallicRoughness.baseColorTexture,model,b);textureInfo(material.pbrMetallicRoughness.metallicRoughnessTexture,model,b);
    textureInfo(material.normalTexture,model,b);textureInfo(material.occlusionTexture,model,b);textureInfo(material.emissiveTexture,model,b);
    finite(material.emissiveFactor,"emissive factor");finite(material.pbrMetallicRoughness.baseColorFactor,"base color");
    for(const auto value:{material.pbrMetallicRoughness.metallicFactor,material.pbrMetallicRoughness.roughnessFactor,
        material.alphaCutoff,material.normalTexture.scale,material.occlusionTexture.strength}) finite(value,"material factor");
    for(const auto& [name,key]:kMaterialTextureFields) if(const auto ext=material.extensions.find(name);ext!=material.extensions.end())
      if(ext->second.Has(key)) extensionTexture(ext->second.Get(key),model,b);
    for(const auto* map:{&material.values,&material.additionalValues}) for(const auto& [key,p]:*map) {
      b.text(key);b.text(p.string_value);finite(p.number_array,"legacy material");b.refs(p.number_array.size());
      for(const auto& [field,value]:p.json_double_value) {b.text(field);finite(value,"legacy material");b.bytes(16);}
      if(p.has_number_value) finite(p.number_value,"legacy material");
      if(const auto found=p.json_double_value.find("index");found!=p.json_double_value.end()) {
        if(!legacyTexture(key)) fail("legacy material","unsupported texture-reference parameter: "+key);
        if(std::trunc(found->second)!=found->second || found->second<0 || found->second>INT_MAX) fail("legacy material","invalid texture index");
        index(int(found->second),model.textures.size(),"legacy texture");b.refs(1);
      }
    }
  }
  for(const auto& mesh:model.meshes) {
    property(mesh,model,b,"mesh",{kGltfXmpExtension});finite(mesh.weights,"mesh weights");b.refs(mesh.weights.size());
    charge(b.sum.primitives,mesh.primitives.size(),lim.maxPrimitives,"primitives");
    for(const auto& primitive:mesh.primitives) {
      property(primitive,model,b,"primitive",{kVariants});
      if(primitive.mode!=TINYGLTF_MODE_TRIANGLES && primitive.mode!=-1) fail("primitive","only normalized TRIANGLES are supported");
      index(primitive.material,model.materials.size(),"primitive material",true);index(primitive.indices,model.accessors.size(),"primitive indices",true);b.refs(2);
      for(const auto& [semantic,id]:primitive.attributes) {b.text(semantic);index(id,model.accessors.size(),"primitive attribute");b.refs(1);}
      for(const auto& target:primitive.targets) for(const auto& [semantic,id]:target) {b.text(semantic);index(id,model.accessors.size(),"morph attribute");b.refs(1);}
    }
  }
  for(const auto& node:model.nodes) {
    property(node,model,b,"node",{kPunctual,kInstancing,kGltfIesExtension,kGltfXmpExtension,"KHR_node_visibility","KHR_node_selectability","KHR_node_hoverability",
      "KHR_lights_area","EXT_lights_area","WEBGI_lights_area","WEBGI_area_light"});
    if(node.emitter!=-1 || !node.lods.empty()) fail("node","audio/LOD references need a merge handler");
    index(node.mesh,model.meshes.size(),"node mesh",true);index(node.skin,model.skins.size(),"node skin",true);
    index(node.camera,model.cameras.size(),"node camera",true);index(node.light,model.lights.size(),"node light",true);b.refs(node.children.size()+4);
    for(const auto* values:{&node.matrix,&node.translation,&node.rotation,&node.scale,&node.weights}) {finite(*values,"node transform/weights");b.refs(values->size());}
    if((!node.matrix.empty() && node.matrix.size()!=16) || (!node.translation.empty() && node.translation.size()!=3) ||
       (!node.rotation.empty() && node.rotation.size()!=4) || (!node.scale.empty() && node.scale.size()!=3) ||
       (!node.matrix.empty() && (!node.translation.empty() || !node.rotation.empty() || !node.scale.empty()))) fail("node","invalid matrix/TRS shape");
    for(const auto& [name,value]:node.extensions) {
      if(name==kPunctual) {const auto light=integer(field(value,"light",name),model.lights.size(),name);if(node.light>=0 && node.light!=light) fail(name,"typed and extension bindings differ");b.refs(1);}
      else if(indexedLight(name)) {const auto found=out.extensionLightCounts.find(name);if(found==out.extensionLightCounts.end()) fail(name,"missing root light table");integer(field(value,"light",name),found->second,name);b.refs(1);}
      else if(name==kInstancing) for(const auto& [semantic,id]:object(field(value,"attributes",name),name)) {b.text(semantic);integer(id,model.accessors.size(),name);b.refs(1);}
    }
  }
  for(const auto& skin:model.skins) {
    property(skin,model,b,"skin");index(skin.skeleton,model.nodes.size(),"skin skeleton",true);index(skin.inverseBindMatrices,model.accessors.size(),"inverse binds",true);b.refs(skin.joints.size()+2);
    if(skin.joints.empty()) fail("skin","empty joint table");std::set<int> joints;
    for(const auto joint:skin.joints) {
      if(joints.size()%1024==0) checkModelImportCancellation(b.stop);
      index(joint,model.nodes.size(),"skin joint");
      if(!joints.insert(joint).second) fail("skin","duplicate joint");
    }
  }
  for(const auto& camera:model.cameras) {
    property(camera,model,b,"camera");b.text(camera.type);property(camera.perspective,model,b,"perspective camera");property(camera.orthographic,model,b,"orthographic camera");
    for(const auto v:{camera.perspective.aspectRatio,camera.perspective.yfov,camera.perspective.znear,camera.perspective.zfar,
        camera.orthographic.xmag,camera.orthographic.ymag,camera.orthographic.znear,camera.orthographic.zfar}) finite(v,"camera");
  }
  for(const auto& light:model.lights) {
    property(light,model,b,"light",{"KHR_lights_area","EXT_lights_area","WEBGI_lights_area","WEBGI_area_light"});property(light.spot,model,b,"spot light");
    finite(light.color,"light color");finite(light.intensity,"light intensity");finite(light.range,"light range");
    finite(light.spot.innerConeAngle,"spot inner cone");finite(light.spot.outerConeAngle,"spot outer cone");b.text(light.type);
  }
  for(const auto& animation:model.animations) {
    property(animation,model,b,"animation",{kGltfXmpExtension});tableCount(animation.samplers.size(),lim,"animation samplers");
    charge(b.sum.animationChannels,animation.channels.size(),lim.maxAnimationChannels,"animation channels");
    for(const auto& sampler:animation.samplers) {property(sampler,model,b,"animation sampler");b.text(sampler.interpolation);index(sampler.input,model.accessors.size(),"animation input");index(sampler.output,model.accessors.size(),"animation output");b.refs(2);}
    for(const auto& channel:animation.channels) {
      property(channel,model,b,"animation channel");b.text(channel.target_path);index(channel.sampler,animation.samplers.size(),"animation sampler");b.refs(1);
      b.value(channel.target_extras);b.text(channel.target_extras_json_string);b.text(channel.target_extensions_json_string);
      extensions(channel.target_extensions,model,b,"animation target",{kPointer});
      if(channel.target_path!="pointer") {index(channel.target_node,model.nodes.size(),"animation node");b.refs(1);}
      charge(b.sum.animationDecodedBytes,channelDecodedBytes(model,animation,channel),lim.maxAnimationDecodedBytes,"animation expansion");
    }
  }
  // Potentially allocating libraries run only after the complete metadata,
  // primitive/reference and channel-expansion preflight above.
  (void)ParseMaterialVariants(model,b.stop);
  checkModelImportCancellation(b.stop);
  // Reject channels the runtime would otherwise diagnose/omit, never lose them.
  const auto library=ParseAnimations(model);
  checkModelImportCancellation(b.stop);
  if(!library.diagnostics.empty()) fail(label,"unsupported animation target: "+library.diagnostics.front().message);
  return out;
}

std::vector<int32_t> identityMap(size_t offset,size_t count,const GltfMergeLimits& limits,std::string_view label) {
  tableCount(add(offset,count,label),limits,label);
  std::vector<int32_t> out;out.reserve(count);
  for(size_t i=0;i<count;++i) out.push_back(int32_t(offset+i)); return out;
}
int translated(int id,const std::vector<int32_t>& map,std::string_view label,bool optional=false) {
  if(optional && id==-1) return -1; return map[size_t(index(id,map.size(),label))];
}
void translate(Value& value,const std::vector<int32_t>& map,std::string_view label) {
  value=Value(translated(integer(value,map.size(),label),map,label));
}
template<class T> void translateInfo(T& info,const GltfMergeIndexMaps& maps) {
  info.index=translated(info.index,maps.textures,"material texture",true);
}
void materialTextures(tinygltf::Material& material,const GltfMergeIndexMaps& maps) {
  translateInfo(material.pbrMetallicRoughness.baseColorTexture,maps);
  translateInfo(material.pbrMetallicRoughness.metallicRoughnessTexture,maps);
  translateInfo(material.normalTexture,maps);translateInfo(material.occlusionTexture,maps);translateInfo(material.emissiveTexture,maps);
  for(const auto& [name,key]:kMaterialTextureFields) if(auto ext=material.extensions.find(name);ext!=material.extensions.end())
    if(ext->second.Has(key)) translate(ext->second.Get<Object>().at(key).Get<Object>().at("index"),maps.textures,"extension texture");
  // TinyGLTF's legacy duplicate core maps are kept consistent, never walked by
  // arbitrary JSON key. Unknown legacy parameter payloads remain unchanged.
  for(auto* values:{&material.values,&material.additionalValues})
    for(auto& [key,parameter]:*values) if(legacyTexture(key))
      if(auto id=parameter.json_double_value.find("index");id!=parameter.json_double_value.end()) {
        if(!std::isfinite(id->second) || std::trunc(id->second)!=id->second || id->second<0 || id->second>INT_MAX)
          fail("legacy texture","invalid index");
        id->second=double(translated(int(id->second),maps.textures,"legacy texture"));
      }
  material.extensions_json_string.clear();
}

struct PointerIdentity {std::string prefix,suffix;int source{};enum Kind {Node,Material,Camera,Light} kind{};};
PointerIdentity pointerIdentity(const std::string& pointer) {
  // ParseAnimations has validated the complete RFC6901 target and property.
  PointerIdentity out;size_t begin{};
  if(pointer.starts_with("/nodes/")) {out.kind=PointerIdentity::Node;out.prefix="/nodes/";}
  else if(pointer.starts_with("/materials/")) {out.kind=PointerIdentity::Material;out.prefix="/materials/";}
  else if(pointer.starts_with("/cameras/")) {out.kind=PointerIdentity::Camera;out.prefix="/cameras/";}
  else if(pointer.starts_with("/extensions/KHR_lights_punctual/lights/")) {out.kind=PointerIdentity::Light;out.prefix="/extensions/KHR_lights_punctual/lights/";}
  else fail("animation pointer","unsupported object identity");
  begin=out.prefix.size();const auto end=pointer.find('/',begin);
  if(end==std::string::npos) fail("animation pointer","missing property");
  const auto parsed=std::from_chars(pointer.data()+begin,pointer.data()+end,out.source);
  if(parsed.ec!=std::errc{} || parsed.ptr!=pointer.data()+end || out.source<0) fail("animation pointer","invalid object identity");
  out.suffix=pointer.substr(end);return out;
}
std::optional<PointerIdentity> channelPointer(const tinygltf::AnimationChannel& channel) {
  if(channel.target_path!="pointer") return {};
  const auto found=channel.target_extensions.find(kPointer);
  if(found==channel.target_extensions.end()) fail("animation pointer","missing target extension");
  const auto& pointer=field(found->second,"pointer","animation pointer");
  if(!pointer.IsString()) fail("animation pointer","expected a string");
  return pointerIdentity(pointer.Get<std::string>());
}
void channelTarget(tinygltf::AnimationChannel& channel,const GltfMergeIndexMaps& maps,
    const std::vector<int32_t>* placement=nullptr) {
  if(const auto pointer=channelPointer(channel)) {
    const std::vector<int32_t>* map{};
    switch(pointer->kind) {
      case PointerIdentity::Node:map=placement?placement:&maps.nodes;break;
      case PointerIdentity::Material:map=&maps.materials;break;
      case PointerIdentity::Camera:map=&maps.cameras;break;
      case PointerIdentity::Light:map=&maps.lights;break;
    }
    const auto id=translated(pointer->source,*map,"animation pointer");
    if(id<0) fail("animation pointer","missing placement target");
    channel.target_extensions.at(kPointer).Get<Object>()["pointer"]=Value(pointer->prefix+std::to_string(id)+pointer->suffix);
    channel.target_extensions_json_string.clear();
  } else channel.target_node=translated(channel.target_node,placement?*placement:maps.nodes,"animation node");
}
std::optional<int> clonedTarget(const tinygltf::AnimationChannel& channel,const GltfMergePlacement& placement) {
  int node=channel.target_node;bool weights=channel.target_path=="weights";
  if(const auto pointer=channelPointer(channel)) {
    if(pointer->kind!=PointerIdentity::Node) return {};
    node=pointer->source;weights=pointer->suffix=="/weights" || pointer->suffix.starts_with("/weights/");
  }
  if(node<0 || size_t(node)>=placement.nodes.size() || placement.nodes[size_t(node)]<0) return {};
  if(weights && placement.contextOnly[size_t(node)]) return {};
  return node;
}

Value opaque(const Value& base,const Value& incoming,std::string_view label) {
  if(incoming==Value{}) return base;
  if(base==Value{} || base==incoming) return incoming;
  fail(label,"conflicting root metadata cannot be represented without a policy");
}
Object rootHeader(const tinygltf::Model& model,const char* name,const char* entries) {
  if(const auto found=model.extensions.find(name);found!=model.extensions.end()) {
    auto header=object(found->second,name);header.erase(entries);return header;
  }
  return {};
}
Object combinedHeader(const tinygltf::Model& base,const tinygltf::Model& incoming,const char* name,const char* entries) {
  const auto a=rootHeader(base,name,entries),b=rootHeader(incoming,name,entries);
  if(a.empty()) return b; if(b.empty() || a==b) return a;
  fail(name,"conflicting root extension metadata");
}
Value::Array rootArray(const tinygltf::Model& model,const char* name,const char* entries) {
  if(const auto found=model.extensions.find(name);found!=model.extensions.end()) return array(field(found->second,entries,name),name);
  return {};
}
Value numbers(const std::vector<double>& values) {Array out;out.reserve(values.size());for(auto v:values) out.emplace_back(v);return Value(std::move(out));}
Value lightValue(const tinygltf::Light& light,const Value* original) {
  Object out=original?object(*original,"light"):Object{};
  out["type"]=Value(light.type);
  if(!light.name.empty() || out.contains("name")) out["name"]=Value(light.name);
  if(!light.color.empty()) out["color"]=numbers(light.color);
  if(light.intensity!=1.0 || out.contains("intensity")) out["intensity"]=Value(light.intensity);
  if(light.range!=0 || out.contains("range")) {if(light.range!=0) out["range"]=Value(light.range);else out.erase("range");}
  if(light.type=="spot") {
    Object spot;
    if(auto it=out.find("spot");it!=out.end()) spot=object(it->second,"spot");
    spot["innerConeAngle"]=Value(light.spot.innerConeAngle);spot["outerConeAngle"]=Value(light.spot.outerConeAngle);
    if(light.spot.extras!=Value{}) spot["extras"]=light.spot.extras;
    if(!light.spot.extensions.empty()) spot["extensions"]=Value(light.spot.extensions);
    out["spot"]=Value(std::move(spot));
  }
  if(light.extras!=Value{}) out["extras"]=light.extras;
  if(!light.extensions.empty()) out["extensions"]=Value(light.extensions);
  return Value(std::move(out));
}
void mergeRoot(tinygltf::Model& output,const tinygltf::Model& base,const tinygltf::Model& incoming) {
  output.extras=opaque(base.extras,incoming.extras,"root extras");
  output.asset.extras=opaque(base.asset.extras,incoming.asset.extras,"asset extras");
  if(base.asset.copyright.empty()) output.asset.copyright=incoming.asset.copyright;
  else if(!incoming.asset.copyright.empty() && base.asset.copyright!=incoming.asset.copyright)
    fail("asset copyright","conflicting values need an explicit preservation policy");
  if(output.asset.minVersion.empty()) output.asset.minVersion=incoming.asset.minVersion;
  for(const auto* names:{&incoming.extensionsUsed,&incoming.extensionsRequired}) {
    auto& dest=names==&incoming.extensionsUsed?output.extensionsUsed:output.extensionsRequired;
    for(const auto& name:*names) if(std::ranges::find(dest,name)==dest.end()) dest.push_back(name);
  }
  if(!base.lights.empty() || !incoming.lights.empty() || base.extensions.contains(kPunctual) || incoming.extensions.contains(kPunctual)) {
    auto header=combinedHeader(base,incoming,kPunctual,"lights");Array lights;
    for(const auto* source:{&base,&incoming}) {
      const auto raw=rootArray(*source,kPunctual,"lights");
      for(size_t i=0;i<source->lights.size();++i) lights.push_back(lightValue(source->lights[i],raw.empty()?nullptr:&raw[i]));
    }
    header["lights"]=Value(std::move(lights));output.extensions[kPunctual]=Value(std::move(header));
    if(std::ranges::find(output.extensionsUsed,std::string(kPunctual))==output.extensionsUsed.end()) output.extensionsUsed.emplace_back(kPunctual);
  }
  for(const auto name:kIndexedLights) if(base.extensions.contains(name) || incoming.extensions.contains(name)) {
    auto header=combinedHeader(base,incoming,name,"lights");auto values=rootArray(base,name,"lights");const auto added=rootArray(incoming,name,"lights");
    if (name==kGltfIesExtension && values.size()+added.size()>kIesMaximumProfiles) fail(name,"profile count exceeds the renderer limit");
    values.insert(values.end(),added.begin(),added.end());header["lights"]=Value(std::move(values));output.extensions[name]=Value(std::move(header));
  }
  if(base.extensions.contains(kVariants) || incoming.extensions.contains(kVariants)) {
    auto header=combinedHeader(base,incoming,kVariants,"variants");auto values=rootArray(base,kVariants,"variants");const auto added=rootArray(incoming,kVariants,"variants");
    values.insert(values.end(),added.begin(),added.end());header["variants"]=Value(std::move(values));output.extensions[kVariants]=Value(std::move(header));
  }
  output.extensions_json_string.clear();output.extras_json_string.clear();output.asset.extras_json_string.clear();
}

std::optional<Value> mergeBehavior(const tinygltf::Model& base, const tinygltf::Model& incoming,
    const GltfMergePlan& plan, size_t totalNodes) {
  const bool destinationGraph = base.extensions.contains(kGltfInteractivityExtension);
  const bool importedGraph = incoming.extensions.contains(kGltfInteractivityExtension);
  if (!destinationGraph && !importedGraph) return {};
  if (destinationGraph && importedGraph)
    fail("KHR_interactivity", "independent behavior graphs cannot share one default runtime");
  const auto& map = plan.imported;
  std::vector<int32_t> weightNodes;
  if (importedGraph) {
    weightNodes = plan.placement.nodes;
    for (size_t i = 0; i < weightNodes.size(); ++i)
      if (plan.placement.contextOnly[i]) weightNodes[i] = -1;
  }
  using Map = InteractivityIndexMapping;
  const auto mapped = [importedGraph](const std::vector<int32_t>& ids, size_t count) {
    return Map{importedGraph ? std::span<const int32_t>(ids) : std::span<const int32_t>{}, count};
  };
  const GltfInteractivityMappings mappings{
      mapped(plan.placement.nodes, totalNodes),
      mapped(weightNodes, totalNodes),
      mapped(map.materials, base.materials.size() + incoming.materials.size()),
      mapped(map.lights, base.lights.size() + incoming.lights.size()),
      mapped(map.cameras, base.cameras.size() + incoming.cameras.size()),
      mapped(map.animations, base.animations.size() + incoming.animations.size()),
  };
  return RemapGltfInteractivity(importedGraph ? incoming : base, mappings);
}

void remapNode(tinygltf::Node& node,const GltfMergeIndexMaps& m) {
  node.mesh=translated(node.mesh,m.meshes,"node mesh",true);node.skin=translated(node.skin,m.skins,"node skin",true);
  node.camera=translated(node.camera,m.cameras,"node camera",true);node.light=translated(node.light,m.lights,"node light",true);
  for(auto& child:node.children) child=translated(child,m.nodes,"node child");
  for(auto& [name,value]:node.extensions) {
    if(name==kPunctual) {auto& id=value.Get<Object>().at("light");translate(id,m.lights,name);node.light=id.Get<int>();}
    else if(indexedLight(name)) translate(value.Get<Object>().at("light"),m.extensionLights.at(name),name);
    else if(name==kInstancing) for(auto& [_,id]:value.Get<Object>().at("attributes").Get<Object>()) translate(id,m.accessors,name);
  }
  node.extensions_json_string.clear();
}
void remapSkin(tinygltf::Skin& skin,const GltfMergeIndexMaps& m,const std::vector<int32_t>* placement=nullptr) {
  const auto& nodes=placement?*placement:m.nodes;
  for(auto& joint:skin.joints) {joint=translated(joint,nodes,"skin joint");if(joint<0) fail("skin joint","missing closure node");}
  skin.skeleton=translated(skin.skeleton,nodes,"skin skeleton",true);
  skin.inverseBindMatrices=translated(skin.inverseBindMatrices,m.accessors,"inverse binds",true);
}

void placement(const tinygltf::Model& incoming,const Audit& audit,size_t selected,
    GltfMergePlan& plan,size_t canonicalSkinCount,const GltfMergeOptions& options,Budget& b) {
  const auto previousReferences = b.sum.references;
  const auto previousMetadata = b.sum.metadataBytes;
  auto& p=plan.placement;const auto size=incoming.nodes.size();
  std::vector<bool> visible(size),closure(size);p.nodes.assign(size,-1);p.contextOnly.assign(size,false);p.skins.assign(incoming.skins.size(),-1);
  std::vector<int> stack(audit.catalog.entries[selected].sourceRoots.begin(),audit.catalog.entries[selected].sourceRoots.end());
  while(!stack.empty()) {checkModelImportCancellation(b.stop);const int id=stack.back();stack.pop_back();if(visible[size_t(id)]) continue;visible[size_t(id)]=true;
    const auto& children=incoming.nodes[size_t(id)].children;stack.insert(stack.end(),children.begin(),children.end());}
  closure=visible;
  const auto include=[&](int id) {
    while(id>=0 && !closure[size_t(id)]) {
      checkModelImportCancellation(b.stop);
      closure[size_t(id)]=true;
      id=audit.parents[size_t(id)];
    }
  };
  for(size_t n=0;n<size;++n) if(visible[n] && incoming.nodes[n].skin>=0) {
    const auto id=size_t(incoming.nodes[n].skin);p.skins[id]=0;const auto& skin=incoming.skins[id];
    for(const auto joint:skin.joints) include(joint);if(skin.skeleton>=0) include(skin.skeleton);
  }
  const auto count=uint64_t(std::ranges::count(closure,true));
  if(count>b.limits.maxPlacementNodes) fail("placement","node limit exceeded");
  b.sum.placementNodes=count;
  size_t next=size_t(plan.parentNode)+1;
  for(size_t n=0;n<size;++n) if(closure[n]) {
    if(next>INT_MAX || next>=b.limits.maxNodes) fail("placement","node limit exceeded");
    p.nodes[n]=int32_t(next++);p.contextOnly[n]=!visible[n];
    property(incoming.nodes[n],incoming,b,"placement node",{kPunctual,kInstancing,kGltfXmpExtension,"KHR_node_visibility","KHR_node_selectability","KHR_node_hoverability","KHR_lights_area","EXT_lights_area","WEBGI_lights_area","WEBGI_area_light"});
    b.refs(incoming.nodes[n].children.size()+incoming.nodes[n].matrix.size()+incoming.nodes[n].translation.size()+incoming.nodes[n].rotation.size()+incoming.nodes[n].scale.size()+incoming.nodes[n].weights.size()+4);
  }
  size_t nextSkin=canonicalSkinCount;
  for(size_t i=0;i<p.skins.size();++i) if(p.skins[i]==0) {if(nextSkin>INT_MAX) fail("placement skins","index overflow");p.skins[i]=int32_t(nextSkin++);property(incoming.skins[i],incoming,b,"placement skin");b.refs(incoming.skins[i].joints.size()+2);}
  tableCount(nextSkin,b.limits,"placement skins");
  for(const auto& animation:incoming.animations) for(const auto& channel:animation.channels) if(clonedTarget(channel,p)) {
    charge(b.sum.clonedChannels,1,b.limits.maxClonedChannels,"cloned animation channels");
    charge(b.sum.animationChannels,1,b.limits.maxAnimationChannels,"animation channels");
    property(channel,incoming,b,"placed animation channel");b.value(channel.target_extras);b.text(channel.target_path);
    extensions(channel.target_extensions,incoming,b,"placed animation target",{kPointer});b.refs(2);
    charge(b.sum.animationDecodedBytes,channelDecodedBytes(incoming,animation,channel),b.limits.maxAnimationDecodedBytes,"placed animation expansion");
  }
  uint64_t roots=0;for(size_t n=0;n<size;++n) if(closure[n] && (audit.parents[n]<0 || !closure[size_t(audit.parents[n])])) ++roots;
  b.objects(1);b.text(options.parentName);b.refs(roots+16);
  b.sum.placementReferences = b.sum.references - previousReferences;
  b.sum.placementMetadataBytes = b.sum.metadataBytes - previousMetadata;
}

} // namespace

bool GltfMergeSupportsExtension(std::string_view name) { return supportedDeclaration(name); }

GltfMergePlan PlanGltfMerge(const tinygltf::Model& base,const tinygltf::Model& incoming,
    const GltfMergeOptions& options,const GltfMergeLimits& limits,const std::stop_token& stop) try {
  checkModelImportCancellation(stop);
  Budget budget{limits,stop};
  if(!limits.maxTableEntries || !limits.maxMetadataBytes || !limits.maxMetadataDepth) fail("limits","zero budget");
  for(const auto [a,b]:{std::pair{base.buffers.size(),incoming.buffers.size()},
      std::pair{base.bufferViews.size(),incoming.bufferViews.size()},std::pair{base.accessors.size(),incoming.accessors.size()},
      std::pair{base.images.size(),incoming.images.size()},std::pair{base.samplers.size(),incoming.samplers.size()},
      std::pair{base.textures.size(),incoming.textures.size()},std::pair{base.materials.size(),incoming.materials.size()},
      std::pair{base.meshes.size(),incoming.meshes.size()},std::pair{base.nodes.size(),incoming.nodes.size()},
      std::pair{base.skins.size(),incoming.skins.size()},std::pair{base.cameras.size(),incoming.cameras.size()},
      std::pair{base.lights.size(),incoming.lights.size()},std::pair{base.animations.size(),incoming.animations.size()}})
    tableCount(add(a,b,"combined tables"),limits,"combined tables");
  if(add(add(base.nodes.size(),incoming.nodes.size(),"combined nodes"),1,"combined nodes")>limits.maxNodes)
    fail("combined nodes","node limit exceeded");
  if(add(std::max(size_t{1},base.scenes.size()),std::max(size_t{1},incoming.scenes.size()),"combined scenes")>limits.maxScenes)
    fail("combined scenes","scene limit exceeded");
  const auto a=audit(base,budget,"destination",true);
  budget.opaqueDestinationExtensions.clear();
  const auto b=audit(incoming,budget,"incoming");
  const auto dest=options.destinationScene.value_or(a.catalog.defaultScene),selected=options.incomingScene.value_or(b.catalog.defaultScene);
  if(dest>=a.catalog.entries.size() || selected>=b.catalog.entries.size()) fail("scene selection","index out of bounds");
  if(options.placementMatrix) ValidateGltfMergePlacement(*options.placementMatrix);
  GltfMergePlan plan;auto& m=plan.imported;
  m.buffers=identityMap(base.buffers.size(),incoming.buffers.size(),limits,"buffers");
  m.bufferViews=identityMap(base.bufferViews.size(),incoming.bufferViews.size(),limits,"buffer views");
  m.accessors=identityMap(base.accessors.size(),incoming.accessors.size(),limits,"accessors");
  m.images=identityMap(base.images.size(),incoming.images.size(),limits,"images");m.samplers=identityMap(base.samplers.size(),incoming.samplers.size(),limits,"samplers");
  m.textures=identityMap(base.textures.size(),incoming.textures.size(),limits,"textures");m.materials=identityMap(base.materials.size(),incoming.materials.size(),limits,"materials");
  m.meshes=identityMap(base.meshes.size(),incoming.meshes.size(),limits,"meshes");m.nodes=identityMap(base.nodes.size(),incoming.nodes.size(),limits,"nodes");
  m.skins=identityMap(base.skins.size(),incoming.skins.size(),limits,"skins");m.cameras=identityMap(base.cameras.size(),incoming.cameras.size(),limits,"cameras");
  m.lights=identityMap(base.lights.size(),incoming.lights.size(),limits,"lights");m.animations=identityMap(base.animations.size(),incoming.animations.size(),limits,"animations");
  m.scenes=identityMap(a.catalog.entries.size(),b.catalog.entries.size(),limits,"scenes");m.variants=identityMap(a.variants,b.variants,limits,"variants");
  for(const auto& [name,count]:b.extensionLightCounts) {const auto found=a.extensionLightCounts.find(name);m.extensionLights[name]=identityMap(found==a.extensionLightCounts.end()?0:found->second,count,limits,name);}
  m.primitives.resize(incoming.meshes.size());
  for(size_t mesh=0;mesh<incoming.meshes.size();++mesh) for(size_t primitive=0;primitive<incoming.meshes[mesh].primitives.size();++primitive) {
    checkModelImportCancellation(stop);
    if(primitive>INT_MAX) fail("primitive","index overflow");m.primitives[mesh].push_back({m.meshes[mesh],int32_t(primitive)});
  }
  const auto canonicalNodes=add(base.nodes.size(),incoming.nodes.size(),"nodes");
  if(canonicalNodes>=INT_MAX || canonicalNodes>=limits.maxNodes) fail("nodes","no room for placement parent");
  plan.parentNode=int32_t(canonicalNodes);plan.destinationScene=int32_t(dest);plan.incomingScene=m.scenes[selected];
  // Only counts are needed before any payload-bearing model copy.
  placement(incoming,b,selected,plan,base.skins.size()+incoming.skins.size(),options,budget);
  const auto totalNodes=add(canonicalNodes,add(budget.sum.placementNodes,1,"nodes"),"nodes");
  if(totalNodes>limits.maxNodes || totalNodes>limits.maxTableEntries || totalNodes>INT_MAX) fail("nodes","combined node limit exceeded");
  if(m.scenes.size()+a.catalog.entries.size()>limits.maxScenes) fail("scenes","combined scene limit exceeded");
  budget.sum.peakPayloadBytes=mul(budget.sum.payloadBytes,2,"resident payloads");
  if(budget.sum.peakPayloadBytes>limits.maxResidentBytes) fail("resident payloads","inputs plus output exceed aggregate limit");
  charge(budget.sum.rootReferences,1,limits.maxRootReferences,"placement scene root");budget.refs(1);
  // Validate all merge-policy conflicts before copying resident payloads.
  tinygltf::Model headers;mergeRoot(headers,base,incoming);
  auto behavior = mergeBehavior(base, incoming, plan, size_t(totalNodes));
  if (behavior) headers.extensions.emplace(kGltfInteractivityExtension, std::move(*behavior));
  // Root representations can coexist with TinyGLTF's typed light table. Charge
  // the generated representation and retained asset provenance before copying.
  for(const auto& [name,value]:headers.extensions) {budget.text(name);budget.value(value);}
  property(incoming.asset,incoming,budget,"incoming asset provenance",{kGltfXmpExtension});
  auto xmp = ComposeGltfXmpPackets(base, incoming, limits.maxMetadataDepth);
  budget.text(xmp.rootExtensionsJson);
  const auto incomingAssetXmp = incoming.asset.extensions.find(kGltfXmpExtension);
  if (incomingAssetXmp != incoming.asset.extensions.end()) {
    budget.text(kGltfXmpExtension);
    budget.value(incomingAssetXmp->second);
  }
  checkModelImportCancellation(stop);
  plan.model=base;
  checkModelImportCancellation(stop);
  auto& out=plan.model;mergeRoot(out,base,incoming);plan.incomingAsset=incoming.asset;
  if (base.extensions.contains(kGltfXmpExtension) || incoming.extensions.contains(kGltfXmpExtension))
    out.extensions.insert_or_assign(kGltfXmpExtension, Value(Object{}));
  if (!xmp.rootExtensionsJson.empty()) out.extensions_json_string = std::move(xmp.rootExtensionsJson);
  if (auto graph = headers.extensions.extract(kGltfInteractivityExtension); !graph.empty())
    out.extensions.insert_or_assign(kGltfInteractivityExtension, std::move(graph.mapped()));
  if(out.scenes.empty()) {tinygltf::Scene scene;scene.name="Scene";scene.nodes.assign(a.catalog.entries[0].sourceRoots.begin(),a.catalog.entries[0].sourceRoots.end());out.scenes.push_back(std::move(scene));}
  for(const auto& value:incoming.buffers) {
    checkModelImportCancellation(stop);
    out.buffers.push_back(value);
  }
  for(auto value:incoming.bufferViews) {value.buffer=translated(value.buffer,m.buffers,"buffer view");out.bufferViews.push_back(std::move(value));}
  for(auto value:incoming.accessors) {value.bufferView=translated(value.bufferView,m.bufferViews,"accessor",true);if(value.sparse.isSparse) {value.sparse.indices.bufferView=translated(value.sparse.indices.bufferView,m.bufferViews,"sparse indices");value.sparse.values.bufferView=translated(value.sparse.values.bufferView,m.bufferViews,"sparse values");}out.accessors.push_back(std::move(value));}
  for(const auto& image:incoming.images) {
    checkModelImportCancellation(stop);
    auto value=image;
    value.bufferView=translated(value.bufferView,m.bufferViews,"image",true);
    out.images.push_back(std::move(value));
  }
  for(auto& image:out.images) if(!image.image.empty()) image.uri.clear();
  out.samplers.insert(out.samplers.end(),incoming.samplers.begin(),incoming.samplers.end());
  for(auto value:incoming.textures) {value.source=translated(value.source,m.images,"texture image",true);value.sampler=translated(value.sampler,m.samplers,"texture sampler",true);for(auto& [name,ext]:value.extensions) translate(ext.Get<Object>().at("source"),m.images,name);value.extensions_json_string.clear();out.textures.push_back(std::move(value));}
  for(auto value:incoming.materials) {materialTextures(value,m);out.materials.push_back(std::move(value));}
  for(auto value:incoming.meshes) {
    for(auto& primitive:value.primitives) {
      checkModelImportCancellation(stop);
      primitive.material=translated(primitive.material,m.materials,"primitive material",true);primitive.indices=translated(primitive.indices,m.accessors,"primitive indices",true);
      for(auto& [_,id]:primitive.attributes) id=translated(id,m.accessors,"attribute");for(auto& target:primitive.targets) for(auto& [_,id]:target) id=translated(id,m.accessors,"morph attribute");
      if(auto ext=primitive.extensions.find(kVariants);ext!=primitive.extensions.end()) for(auto& mapping:ext->second.Get<Object>().at("mappings").Get<Array>()) {
        translate(mapping.Get<Object>().at("material"),m.materials,"variant material");for(auto& id:mapping.Get<Object>().at("variants").Get<Array>()) translate(id,m.variants,"variant");
      }
      primitive.extensions_json_string.clear();
    }
    out.meshes.push_back(std::move(value));
  }
  for(auto value:incoming.nodes) {
    checkModelImportCancellation(stop);
    remapNode(value,m);
    out.nodes.push_back(std::move(value));
  }
  for(auto value:incoming.skins) {remapSkin(value,m);out.skins.push_back(std::move(value));}
  out.cameras.insert(out.cameras.end(),incoming.cameras.begin(),incoming.cameras.end());out.lights.insert(out.lights.end(),incoming.lights.begin(),incoming.lights.end());
  if(incoming.scenes.empty()) {tinygltf::Scene scene;scene.name="Scene";scene.nodes.assign(b.catalog.entries[0].sourceRoots.begin(),b.catalog.entries[0].sourceRoots.end());for(auto& root:scene.nodes) root=translated(root,m.nodes,"scene root");out.scenes.push_back(std::move(scene));}
  else for(auto value:incoming.scenes) {for(auto& root:value.nodes) root=translated(root,m.nodes,"scene root");out.scenes.push_back(std::move(value));}
  tinygltf::Node parent;parent.name=options.parentName;
  if (incomingAssetXmp != incoming.asset.extensions.end())
    parent.extensions.emplace(kGltfXmpExtension, incomingAssetXmp->second);
  if(options.placementMatrix) parent.matrix.assign(options.placementMatrix->begin(),options.placementMatrix->end());
  for(const auto root:b.catalog.entries[selected].sourceRoots) parent.children.push_back(plan.placement.nodes[size_t(root)]);
  for(size_t n=0;n<incoming.nodes.size();++n) if(plan.placement.nodes[n]>=0 && plan.placement.contextOnly[n] &&
      (b.parents[n]<0 || plan.placement.nodes[size_t(b.parents[n])]<0)) parent.children.push_back(plan.placement.nodes[n]);
  out.nodes.push_back(std::move(parent));
  for(size_t n=0;n<incoming.nodes.size();++n) if(plan.placement.nodes[n]>=0) {
    checkModelImportCancellation(stop);
    auto value=incoming.nodes[n];remapNode(value,m);value.children.clear();
    for(const auto child:incoming.nodes[n].children) if(plan.placement.nodes[size_t(child)]>=0) value.children.push_back(plan.placement.nodes[size_t(child)]);
    if(plan.placement.contextOnly[n]) {
      value.mesh=-1;value.skin=-1;value.camera=-1;value.light=-1;value.weights.clear();
      value.extensions.erase(kPunctual);value.extensions.erase(kInstancing);for(const auto name:kIndexedLights) value.extensions.erase(name);
    } else value.skin=translated(incoming.nodes[n].skin,plan.placement.skins,"placed skin",true);
    out.nodes.push_back(std::move(value));
  }
  for(size_t i=0;i<incoming.skins.size();++i) if(plan.placement.skins[i]>=0) {auto value=incoming.skins[i];remapSkin(value,m,&plan.placement.nodes);out.skins.push_back(std::move(value));}
  for(auto value:incoming.animations) {
    for(auto& sampler:value.samplers) {sampler.input=translated(sampler.input,m.accessors,"animation input");sampler.output=translated(sampler.output,m.accessors,"animation output");}
    const auto originalChannels=value.channels.size();
    for(size_t i=0;i<originalChannels;++i) {
      if(i%1024==0) checkModelImportCancellation(stop);
      auto original=value.channels[i];channelTarget(value.channels[i],m);
      if(clonedTarget(original,plan.placement)) {channelTarget(original,m,&plan.placement.nodes);value.channels.push_back(std::move(original));}
    }
    out.animations.push_back(std::move(value));
  }
  out.scenes[dest].nodes.push_back(plan.parentNode);
  out.defaultScene=base.defaultScene<0?0:base.defaultScene;
  if (declared(out, kGltfXmpExtension)) {
    const auto remap = [&](auto& record, bool fromDestination) {
      const auto count = fromDestination ? xmp.destinationPackets : xmp.incomingPackets;
      const auto offset = fromDestination ? 0 : xmp.destinationPackets;
      if (RemapGltfXmpAttachment(record.extensions, count, offset)) record.extensions_json_string.clear();
    };
    const auto remapTable = [&](auto& records, size_t destinationCount) {
      for (size_t i = 0; i < records.size(); ++i) {
        if (i % 1024 == 0) checkModelImportCancellation(stop);
        remap(records[i], i < destinationCount);
      }
    };
    remap(out.asset, true);
    remap(plan.incomingAsset, false);
    remapTable(out.scenes, a.catalog.entries.size());
    remapTable(out.nodes, base.nodes.size());
    remapTable(out.meshes, base.meshes.size());
    remapTable(out.materials, base.materials.size());
    remapTable(out.images, base.images.size());
    remapTable(out.animations, base.animations.size());
  }
  // No publication has occurred; verify the final forest and typed clips too.
  (void)container::scene::BuildGltfSceneCatalog(out,{limits.maxScenes,limits.maxRootReferences},stop);
  checkModelImportCancellation(stop);
  if(!ParseAnimations(out).diagnostics.empty()) fail("output","unsupported animation target");
  checkModelImportCancellation(stop);
  ValidateGltfInteractivity(out);
  checkModelImportCancellation(stop);
  plan.summary=budget.sum;return plan;
} catch(const std::invalid_argument& error) {
  fail("validation",error.what());
}

} // namespace container::geometry::gltf
