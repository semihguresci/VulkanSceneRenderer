#include <Container/utility/MaterialTailReferences.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace container::scene {
namespace {
using Value=tinygltf::Value;
using Object=Value::Object;
using Side=MaterialBindingSide;
enum class Domain { Other,Root,Material,RemovedMaterial,Primitive,Node,Texture,TextureInfo,View,Accessor,Target,Light };
// TinyGLTF uses nontransparent std::map comparators and string-taking Has/Get.
// Linear views avoid allocating a temporary std::string in final predicates.
const Value* member(const Value& value,std::string_view key) noexcept {
  if(!value.IsObject()) return nullptr;
  for(const auto& [name,body]:value.Get<Object>()) if(std::string_view(name)==key) return &body;
  return nullptr;
}
const Value* extension(const tinygltf::ExtensionMap& values,std::string_view key) noexcept {
  for(const auto& [name,body]:values) if(std::string_view(name)==key) return &body;
  return nullptr;
}
constexpr std::array<std::string_view,13> materials{
    "KHR_materials_unlit","KHR_materials_emissive_strength","KHR_materials_ior","KHR_materials_dispersion",
    "KHR_materials_transmission","KHR_materials_volume","KHR_materials_clearcoat","KHR_materials_sheen",
    "KHR_materials_specular","KHR_materials_iridescence","KHR_materials_anisotropy",
    "KHR_materials_diffuse_transmission","KHR_materials_pbrSpecularGlossiness"};
constexpr std::array<std::string_view,4> areas{"KHR_lights_area","EXT_lights_area","WEBGI_lights_area","WEBGI_area_light"};
bool area(std::string_view name) noexcept {return std::ranges::find(areas,name)!=areas.end();}
bool allowed(std::string_view name,Domain domain) noexcept {
  switch(domain) {
    case Domain::RemovedMaterial:return true;
    case Domain::Root:return name=="KHR_materials_variants" || name=="KHR_lights_punctual" || area(name);
    case Domain::Material:return std::ranges::find(materials,name)!=materials.end() || name=="MSFT_lod";
    case Domain::Primitive:return name=="KHR_materials_variants" || name=="KHR_draco_mesh_compression";
    case Domain::Node:return name=="KHR_lights_punctual" || area(name) || name=="EXT_mesh_gpu_instancing" ||
        name=="KHR_node_visibility" || name=="KHR_node_selectability" || name=="KHR_node_hoverability" || name=="MSFT_lod";
    case Domain::Texture:return name=="KHR_texture_basisu" || name=="EXT_texture_webp" || name=="EXT_texture_avif" || name=="MSFT_texture_dds";
    case Domain::TextureInfo:return name=="KHR_texture_transform";
    case Domain::View:return name=="EXT_meshopt_compression" || name=="KHR_meshopt_compression";
    case Domain::Accessor:return name=="KHR_accessor_float64";
    case Domain::Target:return name=="KHR_animation_pointer";
    case Domain::Light:return area(name);
    default:return false;
  }
}
const char* domainName(Domain domain) noexcept {
  switch(domain) {
    case Domain::Root:return "source root";
    case Domain::Material:return "retained material";
    case Domain::Primitive:return "mesh primitive";
    case Domain::Node:return "source node";
    case Domain::Texture:return "texture";
    case Domain::TextureInfo:return "material textureInfo";
    case Domain::View:return "bufferView";
    case Domain::Accessor:return "accessor";
    case Domain::Target:return "animation target";
    case Domain::Light:return "light";
    default:return "retained object";
  }
}
Side opposite(Side side) noexcept {return side==Side::Before?Side::After:Side::Before;}
bool sameContext(MaterialBindingContext a,MaterialBindingContext b) noexcept {
  return a.assetIdentity==b.assetIdentity && a.runtimeMaterialCount==b.runtimeMaterialCount &&
      a.runtimePrimitiveCount==b.runtimePrimitiveCount && a.sourceMaterialToRuntime.size()==b.sourceMaterialToRuntime.size() &&
      std::equal(a.sourceMaterialToRuntime.begin(),a.sourceMaterialToRuntime.end(),b.sourceMaterialToRuntime.begin());
}
bool validLimits(const MaterialTailReferenceLimits& limits) noexcept {
  for(const auto value:{limits.maxTableEntries,limits.maxObjects,limits.maxGraphRows,limits.maxPrimitives,
      limits.maxChannels,limits.maxReferences,limits.maxMetadataValues,limits.maxMetadataBytes,
      limits.maxTraversalSteps,limits.maxOwnedBytes}) if(!value || value==UINT64_MAX) return false;
  return limits.maxMetadataDepth && limits.maxMetadataDepth<=64 && limits.maxDeclarations && limits.maxDeclarations<=1024;
}

struct Projection {
  const MaterialBindingCommand* command{};
  Side target{Side::After};
  bool before{};
};

struct Scan {
  const MaterialReferenceView& view;
  const MaterialTailReferenceLimits& limits;
  Projection projection;
  MaterialTailReferenceStats stats;
  const tinygltf::Model* source{};
  const char* error{};
  std::string_view errorExtension,errorDomain;
  bool fail(const char* message) noexcept {if(!error) error=message;return false;}
  bool unknown(std::string_view name,std::string_view domain) noexcept {
    if(!error) {errorExtension=name;errorDomain=domain;}
    return fail("unknown retained extension may contain material references");
  }
  bool charge(uint64_t& value,uint64_t count,uint64_t cap) noexcept {
    if(count>cap-value) return fail("reference audit traversal/storage limit exceeded");
    value+=count;return true;
  }
  bool work(uint64_t n=1) noexcept {return charge(stats.traversalSteps,n,limits.maxTraversalSteps);}
  bool bytes(uint64_t n) noexcept {return charge(stats.metadataBytes,n,limits.maxMetadataBytes);}
  bool text(std::string_view value) noexcept {return bytes(value.size()) && work();}
  bool refs(uint64_t n=1) noexcept {return charge(stats.references,n,limits.maxReferences) && work(n);}
  bool table(size_t n) noexcept {return n<=limits.maxTableEntries || fail("source table exceeds its audit limit");}
  bool declared(std::string_view name) noexcept {
    for(const auto& entry:source->extensionsUsed) {
      if(!work()) return false;
      if(entry==name) return true;
    }
    return fail("retained extension is absent from extensionsUsed");
  }
  bool nested(const Value& value) noexcept {
    if(!value.IsObject()) return fail("nested extensions must be an object");
    for(const auto& [name,body]:value.Get<Object>()) {
      if(!text(name)) return false;
      if(name!="KHR_texture_transform" && !area(name)) return unknown(name,"nested extension outside extras");
      if(!body.IsObject() || !declared(name)) return fail("malformed nested extension");
    }
    return true;
  }
  bool metadata(const Value& root,bool inspect=true) noexcept {
    struct Frame {
      const Value* value{};size_t next{};Object::const_iterator member;bool entered{},inspect{};
    };
    std::array<Frame,65> stack;size_t depth{};stack[0].value=&root;stack[0].inspect=inspect;
    for(;;) {
      auto& frame=stack[depth];const auto& value=*frame.value;
      if(!frame.entered) {
        if(!work() || !charge(stats.metadataValues,1,limits.maxMetadataValues) || !bytes(sizeof(Value))) return false;
        if((!value.IsString() && !value.Get<std::string>().empty()) ||
            (!value.IsBinary() && !value.Get<std::vector<unsigned char>>().empty()) ||
            (!value.IsArray() && !value.Get<Value::Array>().empty()) ||
            (!value.IsObject() && !value.Get<Object>().empty())) return fail("malformed inactive material metadata storage");
        if(value.IsString()) {if(!text(value.Get<std::string>())) return false;}
        else if(value.IsBinary()) {if(!bytes(value.Get<std::vector<unsigned char>>().size())) return false;}
        else if(value.IsNumber()) {if(!std::isfinite(value.GetNumberAsDouble())) return fail("nonfinite material reference metadata");}
        else if(!value.IsArray() && !value.IsObject() && !value.IsBool() && value.Type()!=tinygltf::NULL_TYPE)
          return fail("unknown material reference metadata type");
        frame.entered=true;frame.next=0;
        if(value.IsObject()) frame.member=value.Get<Object>().begin();
      }
      const Value* child=nullptr;bool childInspect=frame.inspect;
      if(value.IsArray() && frame.next<value.Get<Value::Array>().size()) child=&value.Get<Value::Array>()[frame.next++];
      else if(value.IsObject() && frame.member!=value.Get<Object>().end()) {
        const auto& [name,next]=*frame.member++;if(!text(name)) return false;
        if(name=="extras") childInspect=false;
        else if(frame.inspect && name=="extensions" && !nested(next)) return false;
        child=&next;
      }
      if(child) {
        if(depth==limits.maxMetadataDepth) return fail("material reference metadata depth exceeded");
        auto& next=stack[++depth];next.value=child;next.entered=false;next.inspect=childInspect;
      } else if(!depth) return true;
      else --depth;
    }
  }
  bool extensions(const tinygltf::ExtensionMap& values,Domain domain) noexcept {
    for(const auto& [name,body]:values) {
      if(!text(name)) return false;
      if(!allowed(name,domain)) return unknown(name,domainName(domain));
      if(domain!=Domain::RemovedMaterial && (!body.IsObject() || !declared(name))) return fail("malformed retained extension");
      if(!metadata(body,domain!=Domain::RemovedMaterial)) return false;
    }
    return true;
  }
  template<class T> bool property(const T& object,Domain domain=Domain::Other) noexcept {
    if(!charge(stats.objects,1,limits.maxObjects) || !work()) return false;
    if constexpr(requires {object.name;}) if(!text(object.name)) return false;
    if constexpr(requires {object.extras;}) if(!metadata(object.extras,false)) return false;
    if constexpr(requires {object.extensions;}) if(!extensions(object.extensions,domain)) return false;
    if constexpr(requires {object.extras_json_string;}) if(!text(object.extras_json_string)) return false;
    if constexpr(requires {object.extensions_json_string;}) if(!text(object.extensions_json_string)) return false;
    return true;
  }
  bool materialRef(int id,bool optional,const char* message) noexcept {
    if(!refs()) return false;
    if(optional && id== -1) return true;
    if(id<0 || size_t(id)>=source->materials.size()) return fail("source material reference is outside its table");
    return uint32_t(id)!=view.sourceMaterial || fail(message);
  }
  bool exactInt(const Value& value,size_t count,uint32_t& out) noexcept {
    if(!value.IsInt() || value.Get<int>()<0 || size_t(value.Get<int>())>=count)
      return fail("reference index is not a bounded exact nonnegative integer");
    out=uint32_t(value.Get<int>());return true;
  }
  bool variant(const tinygltf::Primitive& primitive,size_t variants) noexcept {
    const auto* body=extension(primitive.extensions,"KHR_materials_variants");if(!body) return true;
    const auto* mappings=member(*body,"mappings");
    if(!mappings || !mappings->IsArray()) return fail("malformed variant mapping table");
    for(const auto& mapping:mappings->Get<Value::Array>()) {
      const auto* material=member(mapping,"material");const auto* variantIds=member(mapping,"variants");
      if(!work() || !material || !variantIds || !variantIds->IsArray()) return fail("malformed variant material mapping");
      uint32_t id{};
      if(!exactInt(*material,source->materials.size(),id) ||
          !materialRef(int(id),false,"variant mapping still references the removed material")) return false;
      if(variantIds->Get<Value::Array>().empty()) return fail("variant mapping has no variant IDs");
      for(const auto& item:variantIds->Get<Value::Array>())
        if(!refs() || !exactInt(item,variants,id)) return false;
    }
    return true;
  }
  bool pointer(const tinygltf::AnimationChannel& channel) noexcept {
    const auto* body=extension(channel.target_extensions,"KHR_animation_pointer");
    if(!body) return channel.target_path!="pointer" || fail("pointer channel has no pointer");
    const auto* path=member(*body,"pointer");
    if(channel.target_path!="pointer" || channel.target_node!= -1 || !path || !path->IsString()) return fail("malformed animation pointer channel");
    const std::string_view value=path->Get<std::string>();
    if(!text(value) || value.empty() || value[0]!='/') return fail("malformed animation pointer path");
    for(size_t i=0;i<value.size();++i) if(value[i]=='~') {
      if(++i==value.size() || (value[i]!='0' && value[i]!='1')) return fail("invalid JSON pointer escape");
    }
    constexpr std::string_view prefix="/materials/";
    if(!value.starts_with(prefix)) {
      if(value.starts_with("/nodes/") || value.starts_with("/cameras/") ||
          value.starts_with("/extensions/KHR_lights_punctual/lights/") ||
          value.starts_with("/extensions/KHR_lights_area/lights/") ||
          value.starts_with("/extensions/EXT_lights_area/lights/") ||
          value.starts_with("/extensions/WEBGI_lights_area/lights/") ||
          value.starts_with("/extensions/WEBGI_area_light/lights/")) return true;
      return fail("unsupported animation pointer root may contain material references");
    }
    const auto end=value.find('/',prefix.size());const auto number=value.substr(prefix.size(),end==value.npos?value.size()-prefix.size():end-prefix.size());
    if(number.empty() || (number.size()>1 && number.front()=='0')) return fail("material animation pointer index is malformed");
    uint32_t id{};const auto parsed=std::from_chars(number.data(),number.data()+number.size(),id);
    if(parsed.ec!=std::errc{} || parsed.ptr!=number.data()+number.size() || id>uint32_t(INT32_MAX))
      return fail("material animation pointer index is malformed");
    return materialRef(int(id),false,"animation pointer still targets the removed material");
  }
  bool graph() noexcept {
    if(view.graph.nodeCount()>=SceneGraph::kInvalidNode || view.graph.nodeCount()>limits.maxGraphRows) return fail("graph row audit limit exceeded");
    const auto rows=projection.command?projection.command->assignments():std::span<const MaterialBindingAssignment>{};
    size_t next{};
    for(uint32_t id=0;id<view.graph.nodeCount();++id) {
      if(!work() || !charge(stats.graphRows,1,limits.maxGraphRows)) return false;
      const auto* node=view.graph.getNode(id);if(!node) return fail("published graph has a missing dense row");
      uint32_t material=node->materialIndex;
      if(next<rows.size() && rows[next].graphNode==id) {
        if(projection.before) material=projection.target==Side::Before?rows[next].beforeRuntimeMaterial:rows[next].afterRuntimeMaterial;
        ++next;
      }
      if(node->renderable) {
        if(!refs() || node->primitiveIndex>=view.context.runtimePrimitiveCount || material>=view.context.runtimeMaterialCount)
          return fail("published renderable has an invalid primitive/material binding");
        if(material==view.runtimeMaterial) return fail("published renderable still references the removed runtime material");
      }
    }
    return next==rows.size() || fail("prospective material binding has no published graph row");
  }
  bool model(const tinygltf::Model& model) noexcept {
    source=&model;
    if(model.materials.size()!=view.context.sourceMaterialToRuntime.size() ||
        model.extensionsUsed.size()>limits.maxDeclarations || model.extensionsRequired.size()>limits.maxDeclarations)
      return fail("source material/declaration context differs or exceeds its audit limit");
    for(const auto count:{model.buffers.size(),model.bufferViews.size(),model.accessors.size(),model.images.size(),model.samplers.size(),
        model.textures.size(),model.materials.size(),model.meshes.size(),model.nodes.size(),model.skins.size(),model.cameras.size(),
        model.lights.size(),model.animations.size(),model.scenes.size()}) if(!table(count)) return false;
    for(const auto& name:model.extensionsUsed) if(!text(name)) return false;
    for(const auto& name:model.extensionsRequired) if(!text(name) || !declared(name)) return false;
    if(!property(model.asset) || !text(model.asset.version) || !text(model.asset.minVersion) ||
        !text(model.asset.generator) || !text(model.asset.copyright) || !metadata(model.extras,false) ||
        !text(model.extras_json_string) || !text(model.extensions_json_string) || !extensions(model.extensions,Domain::Root)) return false;
    size_t variantCount{};
    if(const auto* body=extension(model.extensions,"KHR_materials_variants")) {
      const auto* variants=member(*body,"variants");
      if(!variants || !variants->IsArray()) return fail("malformed root variants table");
      variantCount=variants->Get<Value::Array>().size();
      if(!table(variantCount)) return false;
    }
    for(const auto& buffer:model.buffers) if(!property(buffer)) return false;
    for(const auto& value:model.bufferViews) if(!property(value,Domain::View)) return false;
    for(const auto& value:model.accessors) {
      if(!property(value,Domain::Accessor)) return false;
      if(value.sparse.isSparse && (!property(value.sparse) || !property(value.sparse.indices) || !property(value.sparse.values))) return false;
    }
    for(const auto& value:model.images) if(!property(value)) return false;
    for(const auto& value:model.samplers) if(!property(value)) return false;
    for(const auto& value:model.textures) if(!property(value,Domain::Texture)) return false;
    for(size_t id=0;id<model.materials.size();++id) {
      const auto& value=model.materials[id];const bool removed=id==view.sourceMaterial;
      const auto textureDomain=removed?Domain::RemovedMaterial:Domain::TextureInfo;
      if(!property(value,removed?Domain::RemovedMaterial:Domain::Material) ||
          !property(value.pbrMetallicRoughness,removed?Domain::RemovedMaterial:Domain::Other) ||
          !property(value.pbrMetallicRoughness.baseColorTexture,textureDomain) ||
          !property(value.pbrMetallicRoughness.metallicRoughnessTexture,textureDomain) ||
          !property(value.normalTexture,textureDomain) || !property(value.occlusionTexture,textureDomain) ||
          !property(value.emissiveTexture,textureDomain)) return false;
      if(!table(value.lods.size())) return false;
      if(!removed) {
        for(const auto lod:value.lods) if(!materialRef(lod,false,"material LOD still references the removed material")) return false;
        if(const auto* body=extension(value.extensions,"MSFT_lod")) {
          const auto* ids=member(*body,"ids");
          if(!ids || !ids->IsArray()) return fail("malformed material LOD IDs");
          for(const auto& item:ids->Get<Value::Array>()) {
            uint32_t target{};
            if(!exactInt(item,model.materials.size(),target) || !materialRef(int(target),false,"raw material LOD still references the removed material")) return false;
          }
        }
      }
      for(const auto* legacy:{&value.values,&value.additionalValues}) for(const auto& [name,parameter]:*legacy) {
        if(!work() || !text(name) || !text(parameter.string_value) || !table(parameter.number_array.size()) ||
            !table(parameter.json_double_value.size()) || !bytes(parameter.number_array.size()*sizeof(double))) return false;
        for(const auto& [key,number]:parameter.json_double_value)
          if(!text(key) || !std::isfinite(number)) return fail("invalid legacy material metadata");
      }
    }
    for(const auto& mesh:model.meshes) {
      if(!property(mesh) || !charge(stats.primitives,mesh.primitives.size(),limits.maxPrimitives)) return false;
      for(const auto& primitive:mesh.primitives)
        if(!property(primitive,Domain::Primitive) || !materialRef(primitive.material,true,"source primitive still references the removed material") ||
            !variant(primitive,variantCount)) return false;
    }
    for(const auto& value:model.nodes) if(!property(value,Domain::Node)) return false;
    for(const auto& value:model.skins) if(!property(value)) return false;
    for(const auto& value:model.cameras) if(!property(value) || !property(value.perspective) || !property(value.orthographic)) return false;
    for(const auto& value:model.lights) if(!property(value,Domain::Light) || !property(value.spot)) return false;
    for(const auto& value:model.scenes) if(!property(value)) return false;
    for(const auto& animation:model.animations) {
      if(!property(animation) || !table(animation.samplers.size()) ||
          !charge(stats.channels,animation.channels.size(),limits.maxChannels)) return false;
      for(const auto& sampler:animation.samplers) if(!property(sampler)) return false;
      for(const auto& channel:animation.channels)
        if(!property(channel) || !text(channel.target_path) || !metadata(channel.target_extras,false) ||
            !text(channel.target_extras_json_string) || !text(channel.target_extensions_json_string) ||
            !extensions(channel.target_extensions,Domain::Target) || !pointer(channel)) return false;
    }
    return true;
  }
  bool derivedVariants() noexcept {
    const auto& library=view.variants;
    if(library.materialCount()!=view.context.sourceMaterialToRuntime.size() || !table(library.names().size()) ||
        !table(library.primitives().size())) return fail("derived material variant context differs");
    for(const auto& name:library.names()) if(!text(name)) return false;
    for(const auto& primitive:library.primitives()) {
      if(!work() || primitive.sourceMesh>=view.authoredSource.meshes.size() ||
          primitive.sourcePrimitive>=view.authoredSource.meshes[primitive.sourceMesh].primitives.size())
        return fail("derived variant primitive identity is invalid");
      if(!materialRef(primitive.defaultMaterial,true,"derived variant default still references the removed material")) return false;
      for(const auto& [variant,material]:primitive.mappings)
        if(!refs() || variant>=library.names().size() || !materialRef(material,false,"derived variant mapping still references the removed material"))
          return fail("derived variant material reference is invalid or retained");
    }
    return true;
  }
  bool run() noexcept {
    if(!validLimits(limits) || !view.context.assetIdentity || !view.context.runtimeMaterialCount ||
        view.context.runtimeMaterialCount>uint32_t(INT32_MAX) ||
        view.context.sourceMaterialToRuntime.size()>size_t(INT32_MAX) || view.context.runtimePrimitiveCount==UINT32_MAX ||
        !table(view.context.sourceMaterialToRuntime.size()) || !table(view.protectedRuntimeMaterials.size()) ||
        view.runtimeMaterial>=view.context.runtimeMaterialCount ||
        (view.sourceMaterial!=UINT32_MAX && (view.sourceMaterial>=view.context.sourceMaterialToRuntime.size() ||
        view.context.sourceMaterialToRuntime[view.sourceMaterial]!=view.runtimeMaterial)))
      return fail("material retirement requires an explicitly mapped source/runtime pair");
    for(size_t i=0;i<view.context.sourceMaterialToRuntime.size();++i) {
      const auto runtime=view.context.sourceMaterialToRuntime[i];if(!work()) return false;
      if(runtime>=view.context.runtimeMaterialCount || (i!=view.sourceMaterial && runtime==view.runtimeMaterial))
        return fail("retained source mapping refers to the removed runtime material");
    }
    for(const auto runtime:view.protectedRuntimeMaterials)
      if(!refs() || runtime>=view.context.runtimeMaterialCount || runtime==view.runtimeMaterial)
        return fail("implicit/default/diagnostic material is protected or invalid");
    if(!graph() || !model(view.authoredSource) || !model(view.evaluatedSource)) return false;
    return derivedVariants();
  }
};
} // namespace

struct MaterialTailReferenceAudit::State {
  const tinygltf::Model* authored{};const tinygltf::Model* evaluated{};
  const SceneGraph* graph{};const container::geometry::gltf::MaterialVariantLibrary* variants{};
  uint64_t graphIdentity{},beforeRevision{},afterRevision{},asset{};
  uint32_t sourceTail{},runtimeTail{},runtimeCount{},primitiveCount{};
  std::vector<uint32_t> mapping,protectedMaterials;
  MaterialBindingCommand command;Side target{Side::After};bool projected{};
  MaterialTailReferenceLimits limits;MaterialTailReferenceStats stats;
};
struct MaterialTailReferenceAccess {
  using State=MaterialTailReferenceAudit::State;
  static const State* state(const MaterialTailReferenceAudit& audit) noexcept {return audit.state_.get();}
  static MaterialTailReferenceAudit make(std::shared_ptr<const State> state) noexcept {
    MaterialTailReferenceAudit result;result.state_=std::move(state);return result;
  }
};
MaterialTailReferenceAudit::operator bool() const noexcept {return bool(state_);}
MaterialTailReferenceStats MaterialTailReferenceAudit::stats() const noexcept {return state_?state_->stats:MaterialTailReferenceStats{};}
uint64_t MaterialTailReferenceAudit::expectedGraphRevision() const noexcept {return state_?state_->beforeRevision:0;}
uint64_t MaterialTailReferenceAudit::candidateGraphRevision() const noexcept {return state_?state_->afterRevision:0;}

MaterialReferenceAudit AuditUnusedMaterialReferences(const MaterialReferenceView& view,
    const PreparedMaterialBindingChange* bindings,const MaterialTailReferenceLimits& limits) {
  if(!validLimits(limits)) throw std::invalid_argument("Material tail audit requires finite limits");
  const auto fail=[](const char* message) {throw std::invalid_argument(std::string("Material tail audit: ")+message);};
  uint64_t owned=sizeof(MaterialTailReferenceAccess::State)+128;
  const auto charge=[&](uint64_t count,uint64_t width) {
    if(owned>limits.maxOwnedBytes || count>(limits.maxOwnedBytes-owned)/width) fail("owned proof storage limit exceeded");
    owned+=count*width;
  };
  charge(view.context.sourceMaterialToRuntime.size(),sizeof(uint32_t));charge(view.protectedRuntimeMaterials.size(),sizeof(uint32_t));
  if(bindings && !CanPublishMaterialBindings(view.graph,*bindings,view.context)) fail("prospective material binding proof is stale or foreign");
  if(bindings) charge(bindings->command().ownedBytes(),1);
  const auto command=bindings?bindings->command():MaterialBindingCommand{};
  const auto target=bindings?bindings->targetSide():Side::After;
  Scan scan{view,limits,{bindings?&command:nullptr,target,true}};
  if(!scan.run()) {
    if(!scan.errorExtension.empty())
      throw std::invalid_argument(std::string("Material tail audit: ")+scan.error+" ["+
          std::string(scan.errorDomain)+": "+std::string(scan.errorExtension.substr(0,256))+
          (scan.errorExtension.size()>256?"...":"")+"]");
    fail(scan.error?scan.error:"reference audit failed");
  }
  auto state=std::make_shared<MaterialTailReferenceAccess::State>();
  state->authored=&view.authoredSource;state->evaluated=&view.evaluatedSource;state->graph=&view.graph;state->variants=&view.variants;
  state->graphIdentity=view.graph.identity();state->beforeRevision=view.graph.revision();
  state->afterRevision=bindings?bindings->candidateGraphRevision():state->beforeRevision;
  state->asset=view.context.assetIdentity;state->sourceTail=view.sourceMaterial;state->runtimeTail=view.runtimeMaterial;
  state->runtimeCount=view.context.runtimeMaterialCount;state->primitiveCount=view.context.runtimePrimitiveCount;
  state->mapping.assign(view.context.sourceMaterialToRuntime.begin(),view.context.sourceMaterialToRuntime.end());
  state->protectedMaterials.assign(view.protectedRuntimeMaterials.begin(),view.protectedRuntimeMaterials.end());
  state->command=command;state->target=target;state->projected=bindings!=nullptr;state->limits=limits;
  state->stats=scan.stats;state->stats.ownedBytes=owned;
  return MaterialTailReferenceAccess::make(std::move(state));
}

bool CanRetireUnusedMaterial(const MaterialReferenceView& view,const MaterialTailReferenceAudit& audit,
    MaterialTailAuditPhase phase) noexcept {
  const auto* state=MaterialTailReferenceAccess::state(audit);
  if(!state || (phase!=MaterialTailAuditPhase::BeforeBindings && phase!=MaterialTailAuditPhase::AfterBindings) ||
      state->authored!=&view.authoredSource || state->evaluated!=&view.evaluatedSource || state->graph!=&view.graph ||
      state->variants!=&view.variants || state->graphIdentity!=view.graph.identity() ||
      view.graph.revision()!=(phase==MaterialTailAuditPhase::BeforeBindings?state->beforeRevision:state->afterRevision) ||
      state->sourceTail!=view.sourceMaterial || state->runtimeTail!=view.runtimeMaterial ||
      !sameContext(view.context,{state->asset,state->mapping,state->runtimeCount,state->primitiveCount}) ||
      view.protectedRuntimeMaterials.size()!=state->protectedMaterials.size() ||
      !std::equal(view.protectedRuntimeMaterials.begin(),view.protectedRuntimeMaterials.end(),state->protectedMaterials.begin())) return false;
  if(state->projected && !MatchesMaterialBindings(view.graph,state->command,
      phase==MaterialTailAuditPhase::BeforeBindings?view.context:state->command.context(state->target),
      phase==MaterialTailAuditPhase::BeforeBindings?opposite(state->target):state->target)) return false;
  Scan scan{view,state->limits,{state->projected?&state->command:nullptr,state->target,phase==MaterialTailAuditPhase::BeforeBindings}};
  return scan.run();
}
namespace {
MaterialReferenceView selected(const MaterialTailReferenceView& view) noexcept {
  return {view.authoredSource,view.evaluatedSource,view.graph,view.variants,view.context,
      view.sourceTail,view.runtimeTail,view.protectedRuntimeMaterials};
}
bool exactTail(const MaterialTailReferenceView& view) noexcept {
  return !view.context.sourceMaterialToRuntime.empty() && view.context.runtimeMaterialCount &&
      view.sourceTail==view.context.sourceMaterialToRuntime.size()-1 &&
      view.runtimeTail==view.context.runtimeMaterialCount-1 &&
      view.context.sourceMaterialToRuntime.back()==view.runtimeTail;
}
}
MaterialTailReferenceAudit AuditMaterialTailRemoval(const MaterialTailReferenceView& view,
    const PreparedMaterialBindingChange* bindings,const MaterialTailReferenceLimits& limits) {
  if(!exactTail(view)) throw std::invalid_argument("Material tail audit requires exact last source/runtime identities");
  return AuditUnusedMaterialReferences(selected(view),bindings,limits);
}
bool CanRemoveMaterialTail(const MaterialTailReferenceView& view,const MaterialTailReferenceAudit& audit,
    MaterialTailAuditPhase phase) noexcept {
  return exactTail(view) && CanRetireUnusedMaterial(selected(view),audit,phase);
}
} // namespace container::scene
