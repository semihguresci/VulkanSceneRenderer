#include "Container/renderer/core/RendererFrontend.h"
#include "RendererScenePublicationProof.h"
#include "Container/renderer/core/RenderTechnique.h"
#include "Container/renderer/bim/BimManager.h"
#include "Container/renderer/culling/GpuCullManager.h"
#include "Container/renderer/lighting/DdgiManager.h"
#include "Container/renderer/lighting/LightingManager.h"
#include "Container/renderer/pipeline/RuntimeShaderCompiler.h"
#include "Container/renderer/platform/VulkanContextInitializer.h"
#include "Container/renderer/raytracing/RayShadowManager.h"
#include "Container/renderer/resources/FrameResourceManager.h"
#include "Container/renderer/resources/FrameResourceRegistry.h"
#include "Container/renderer/scene/PbrAuxiliaryPass.h"
#include "Container/renderer/scene/MeshSceneProviderBindings.h"
#include "Container/renderer/scene/SceneController.h"
#include "Container/renderer/scene/SceneProviderSynchronizer.h"
#include "Container/renderer/shadow/ShadowCullManager.h"
#include "Container/renderer/shadow/ShadowManager.h"
#include "Container/renderer/temporal/TemporalManager.h"
#include "Container/utility/GuiManager.h"
#include "Container/utility/Platform.h"
#include "Container/utility/SceneManager.h"
#include "Container/utility/MaterialSamplerKey.h"
#include "Container/utility/SwapChainManager.h"
#include "Container/utility/VulkanDevice.h"
#include "Container/geometry/GltfTextureResourceReferences.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <charconv>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>
#include <nlohmann/json.hpp>

namespace container::renderer {
using namespace container::geometry::gltf;
namespace {
bool hasPatch(const GltfMaterialPatch& patch) {
  return !patch.values.empty() || !patch.extensions.empty() || !patch.textures.empty() || patch.alphaMode || patch.doubleSided || patch.name;
}
uint64_t materialShadowDrawRevision(std::span<const DrawCommand> draws) {
  uint64_t hash=1469598103934665603ull;
  const auto mix=[&](uint32_t v){hash^=v;hash*=1099511628211ull;};
  mix(uint32_t(draws.size()));for(const auto& d:draws){mix(d.indexCount);mix(d.instanceCount);mix(d.firstIndex);mix(d.objectIndex);}return hash;
}
bool sameInspectorBinding(container::ui::MaterialInspectorTarget a,container::ui::MaterialInspectorTarget b) {
  a.authoredRevision=b.authoredRevision=0;return a==b;
}
bool bindingWithinSelection(const container::scene::SceneGraph& graph,uint32_t selected,uint32_t binding) {
  for(size_t depth=0;depth<=graph.nodeCount();++depth) {
    const auto* node=graph.getNode(binding);
    if(!node || graph.isNodeRetired(binding)) return false;
    if(binding==selected) return true;
    binding=node->parent;
  }
  return false;
}
container::scene::MaterialBindingContext materialBindingContext(const container::scene::SceneManager& manager) {
  if(manager.materialCount()>UINT32_MAX || manager.primitiveRanges().size()>UINT32_MAX)
    throw std::length_error("Material binding identity table exceeds its limit");
  return {manager.materialAssetIdentity(),manager.sourceMaterialRuntimeMapping(),
      uint32_t(manager.materialCount()),uint32_t(manager.primitiveRanges().size())};
}
void validateBindingFrameReuseForUv(const container::scene::SceneManager& manager,
    const container::scene::SceneNode& node,std::optional<uint32_t> uv) {
  if(!uv) return;
  if(*uv>1) throw std::invalid_argument("This normal texture requires an unsupported texture-coordinate set");
  const auto retained=manager.retainedPrimitiveNormalTexCoord(node.primitiveIndex);
  if(!retained)
    throw std::invalid_argument("Normal-mapped assignment requires retained primitive frame provenance");
  // This is the retained geometry basis, independent of today's material. A
  // no-map assignment does not reset it. UV-changing per-binding streams are
  // staged separately before that assignment path can be enabled.
  if(*uv!=*retained)
    throw std::invalid_argument("This assignment requires a new per-surface normal UV frame");
}
void validateBindingFrameReuse(const container::scene::SceneManager& manager,
    const container::scene::SceneNode& node,uint32_t runtime) {
  validateBindingFrameReuseForUv(manager,node,manager.materialNormalTexCoord(runtime));
}
void validateTextureFrameReuse(const container::scene::SceneManager& manager,
    const container::scene::SceneGraph& graph,uint32_t runtime,const tinygltf::Material& material) {
  const auto normal=ReadGltfMaterialTextureBinding(material,GltfMaterialTextureSlot::Normal);
  if(normal.index<0) return;
  // Replacing pixels or editing another slot does not change the normal UV
  // requirement. A map added after absence still needs the retained basis.
  if(const auto previous=manager.materialNormalTexCoord(runtime);previous && *previous==normal.texCoord) return;
  if(graph.nodeCount()>1'000'000) throw std::length_error("Material frame reference traversal exceeds its limit");
  for(uint32_t id=0;id<graph.nodeCount();++id) {
    const auto* node=graph.getNode(id);
    if(node && node->renderable && !graph.isNodeRetired(id) && node->materialIndex==runtime)
      validateBindingFrameReuseForUv(manager,*node,normal.texCoord);
  }
  // An inactive default or variant can become live without a material edit.
  // Check its retained frame too, before publishing a new normal UV choice.
  const auto& source=manager.animationSource();
  const auto& materials=manager.sourceMaterialRuntimeMapping();
  const auto& primitives=manager.sourcePrimitiveToRuntime();
  if(source.meshes.size()>1'000'000 || primitives.size()>1'000'000)
    throw std::length_error("Material frame reference traversal exceeds its limit");
  const auto usesRuntime=[&](int32_t materialId) {
    if(materialId<0) return false;
    if(size_t(materialId)>=materials.size())
      throw std::invalid_argument("A retained primitive's source material is unavailable");
    return materials[size_t(materialId)]==runtime;
  };
  size_t flat{},mappings{};
  for(const auto& mesh:source.meshes) for(const auto& primitive:mesh.primitives) {
    if(flat>=primitives.size())
      throw std::invalid_argument("Retained primitive frame identities are incomplete");
    bool uses=usesRuntime(primitive.material);
    const auto variants=primitive.extensions.find("KHR_materials_variants");
    if(variants!=primitive.extensions.end()) {
      const auto& body=variants->second;
      if(!body.IsObject() || !body.Has("mappings") || !body.Get("mappings").IsArray())
        throw std::invalid_argument("Retained material variant mappings are invalid");
      const auto& rows=body.Get("mappings").Get<tinygltf::Value::Array>();
      if(rows.size()>1'000'000-mappings)
        throw std::length_error("Material frame reference traversal exceeds its limit");
      mappings+=rows.size();
      for(const auto& row:rows) {
        if(!row.IsObject() || !row.Has("material") || !row.Get("material").IsNumber())
          throw std::invalid_argument("A retained variant material identity is invalid");
        const auto id=row.Get("material").GetNumberAsDouble();
        if(!std::isfinite(id) || id<0 || id>INT32_MAX || std::trunc(id)!=id)
          throw std::invalid_argument("A retained variant material identity is invalid");
        uses=usesRuntime(int32_t(id)) || uses;
      }
    }
    if(uses) {
      container::scene::SceneNode retained;
      retained.primitiveIndex=primitives[flat];
      validateBindingFrameReuseForUv(manager,retained,normal.texCoord);
    }
    ++flat;
  }
  if(flat!=primitives.size())
    throw std::invalid_argument("Retained primitive frame identities are inconsistent");
}
int materialTextureImage(const tinygltf::Texture& texture) {
  if(texture.source>=0) return texture.source;
  for(const auto name:{"KHR_texture_basisu","EXT_texture_webp","MSFT_texture_dds"}) {
    const auto found=texture.extensions.find(name);
    if(found==texture.extensions.end() || !found->second.IsObject() || !found->second.Has("source")) continue;
    const auto& value=found->second.Get("source");
    if(!value.IsNumber()) continue;
    const auto index=value.GetNumberAsDouble();
    if(std::isfinite(index) && index>=0 && index<=INT32_MAX && std::trunc(index)==index) return int(index);
  }
  return -1;
}
std::string materialTextureFile(const container::scene::MaterialResourceSourceView& source,uint32_t texture) {
  if(texture>=source.textures.size()) return {};
  const auto& record=source.textures[texture];const auto image=materialTextureImage(record);
  std::string_view text=record.name;
  if(image>=0 && size_t(image)<source.images.size()) {
    const auto& entry=source.images[size_t(image)];
    if(!entry.uri.empty() && !std::string_view(entry.uri).starts_with("data:")) text=entry.uri;
    else if(!entry.name.empty()) text=entry.name;
  }
  if(text.empty()) return image>=0?"Embedded image "+std::to_string(image):"Texture "+std::to_string(texture);
  size_t end=std::min<size_t>(text.size(),1024);
  while(end<text.size() && end && (uint8_t(text[end])&0xc0)==0x80) --end;
  return std::string(text.substr(0,end));
}
container::material::MaterialTextureSamplerDescription materialTextureSampler(
    const container::scene::MaterialResourceSourceView& source,uint32_t texture) {
  container::material::MaterialTextureSamplerDescription result;
  if(texture>=source.textures.size()) return result;
  const auto sampler=source.textures[texture].sampler;
  if(sampler>=0 && size_t(sampler)<source.samplers.size()) {
    const auto& value=source.samplers[size_t(sampler)];result={value.minFilter,value.magFilter,value.wrapS,value.wrapT};
  }
  try {container::material::ValidateMaterialTextureSampler(result);}
  catch(const std::invalid_argument&) {
    const auto policy=container::material::DecodeMaterialSamplerKey(container::material::NormalizeMaterialSamplerKey(
        result.minFilter,result.magFilter,result.wrapS,result.wrapT));
    result={policy.minFilter,policy.magFilter,policy.wrapS,policy.wrapT};
  }
  return result;
}
// BIM materials use the primary manager's runtime IDs, including hidden native
// point/curve objects. Pin that entire domain rather than the visible ray list.
using MaterialExternalProof=detail::ScenePublicationExternalProof;
std::string externalMaterialRetirementReason(const BimManager* bim,
    const container::scene::SceneProviderRegistry* registry,uint32_t forbidden) {
  if(!registry) return "The renderer material reference registry is not ready.";
  if(bim) {
    if(bim->objectData().size()>1'000'000 || bim->elementMetadata().size()>1'000'000)
      return "BIM reference metadata exceeds the material deletion audit limit.";
    for(const auto& object:bim->objectData()) if(object.objectInfo.x==forbidden) return "A BIM object uses this material, including hidden objects.";
    for(const auto row:bim->elementMetadata()) if(row.materialIndex==forbidden) return "Retained BIM metadata uses this material.";
  }
  const auto providers=registry->providers();
  if(providers.size()>16) return "The material provider audit exceeds its limit.";
  for(const auto* provider:providers) {
    if(!provider) return "A material provider is unavailable.";
    const bool primary=dynamic_cast<const container::scene::MeshSceneProvider*>(provider)!=nullptr;
    const bool known=primary || dynamic_cast<const container::scene::BimSceneProvider*>(provider) ||
        dynamic_cast<const container::scene::GaussianSplatSceneProvider*>(provider) || dynamic_cast<const container::scene::RadianceFieldSceneProvider*>(provider);
    if(!known) return "A provider's material reference domain cannot be proved.";
    const auto snapshot=provider->snapshot();
    if(snapshot.triangleBatches.size()>1'000'000) return "Material provider metadata exceeds the audit limit.";
    if(!(primary && snapshot.id.value=="primary-mesh-scene")) for(const auto& batch:snapshot.triangleBatches)
      if(batch.materialIndex>=0 && uint32_t(batch.materialIndex)==forbidden) return "A retained scene provider uses this material.";
  }
  return {};
}
nlohmann::json materialValueJson(const tinygltf::Value& value,uint32_t depth,uint64_t& remaining) {
  if(depth>64 || !remaining) throw std::runtime_error("Material telemetry metadata exceeds its limits");
  --remaining;
  if(value.IsBool()) return value.Get<bool>();
  if(value.IsInt()) return value.Get<int>();
  if(value.IsReal()) return value.Get<double>();
  if(value.IsString()) return value.Get<std::string>();
  if(value.IsBinary()) return value.Get<std::vector<unsigned char>>();
  if(value.IsArray()) {
    auto result=nlohmann::json::array();
    for(const auto& item:value.Get<tinygltf::Value::Array>()) result.push_back(materialValueJson(item,depth+1,remaining));
    return result;
  }
  if(value.IsObject()) {
    auto result=nlohmann::json::object();
    for(const auto& [key,item]:value.Get<tinygltf::Value::Object>()) result[key]=materialValueJson(item,depth+1,remaining);
    return result;
  }
  return nullptr;
}
template<class Record>
void appendMaterialMetadata(nlohmann::json& json,const Record& record,uint64_t& remaining) {
  if(!record.extensions.empty()) {
    auto& extensions=json["extensions"];extensions=nlohmann::json::object();
    for(const auto& [name,value]:record.extensions) extensions[name]=materialValueJson(value,0,remaining);
  }
  if(record.extras.Type()!=tinygltf::NULL_TYPE) json["extras"]=materialValueJson(record.extras,0,remaining);
}
template<class Texture>
nlohmann::json materialTextureJson(const Texture& texture,uint64_t& remaining) {
  nlohmann::json result={{"index",texture.index},{"texCoord",texture.texCoord}};
  appendMaterialMetadata(result,texture,remaining);return result;
}
nlohmann::json materialRecordJson(const tinygltf::Material& material,uint64_t& remaining) {
  const auto& pbr=material.pbrMetallicRoughness;
  nlohmann::json result={{"name",material.name},{"alphaMode",material.alphaMode},
      {"alphaCutoff",material.alphaCutoff},{"doubleSided",material.doubleSided},
      {"emissiveFactor",material.emissiveFactor},{"extensions",nlohmann::json::object()}};
  auto& metallic=result["pbrMetallicRoughness"];
  metallic={{"baseColorFactor",pbr.baseColorFactor},{"metallicFactor",pbr.metallicFactor},{"roughnessFactor",pbr.roughnessFactor}};
  appendMaterialMetadata(metallic,pbr,remaining);
  if(pbr.baseColorTexture.index>=0) metallic["baseColorTexture"]=materialTextureJson(pbr.baseColorTexture,remaining);
  if(pbr.metallicRoughnessTexture.index>=0) metallic["metallicRoughnessTexture"]=materialTextureJson(pbr.metallicRoughnessTexture,remaining);
  if(material.normalTexture.index>=0) {
    result["normalTexture"]=materialTextureJson(material.normalTexture,remaining);
    result["normalTexture"]["scale"]=material.normalTexture.scale;
  }
  if(material.occlusionTexture.index>=0) {
    result["occlusionTexture"]=materialTextureJson(material.occlusionTexture,remaining);
    result["occlusionTexture"]["strength"]=material.occlusionTexture.strength;
  }
  if(material.emissiveTexture.index>=0) result["emissiveTexture"]=materialTextureJson(material.emissiveTexture,remaining);
  appendMaterialMetadata(result,material,remaining);return result;
}
nlohmann::json materialRecordJson(const tinygltf::Material& material) {
  uint64_t remaining=262144;return materialRecordJson(material,remaining);
}
container::scene::TextureResourceSelection resourceSelection(const container::ui::ResourceCatalogTarget& target) {
  using D=container::ui::ResourceCatalogDomain;using K=container::ui::ResourceCatalogKind;
  using Domain=container::util::ResourceDomainKind;using Kind=container::util::ResourceKind;
  Domain domain;switch(target.domain) {case D::Archive:domain=Domain::Archive;break;case D::Evaluated:domain=Domain::Evaluated;break;
    case D::Runtime:domain=Domain::Runtime;break;default:throw std::invalid_argument("Unknown resource catalog domain");}
  Kind kind;switch(target.kind) {case K::Image:kind=Kind::Image;break;case K::Texture:kind=Kind::Texture;break;
    case K::Sampler:kind=Kind::Sampler;break;case K::RuntimeTexture:kind=Kind::RuntimeTexture;break;
    default:throw std::invalid_argument("Unknown resource catalog kind");}
  if((domain==Domain::Runtime)!=(kind==Kind::RuntimeTexture) || target.row>=1'000'000)
    throw std::invalid_argument("Resource catalog target has an invalid identity domain");
  return {domain,kind,target.row};
}
container::util::ResourceHandle resolveResourceTarget(const container::scene::SceneManager& manager,
    const container::ui::ResourceCatalogTarget& target) {
  const auto generation=manager.resourceLifecycleGeneration();const auto selection=resourceSelection(target);
  if(!generation || !generation->valid() || target.assetIdentity!=manager.materialAssetIdentity() ||
      target.resourceRevision!=manager.materialResourceRevision() || !generation->active(selection))
    throw std::invalid_argument("The resource catalog changed; refresh before deleting");
  const auto handle=generation->resource(selection);
  if(!handle || handle.domain().identity()!=target.domainIdentity || handle.incarnation()!=target.incarnation)
    throw std::invalid_argument("The selected resource incarnation is no longer current");
  return handle;
}
bool sameResourceSelection(const container::ui::ResourceCatalogTarget& a,const container::ui::ResourceCatalogTarget& b) noexcept {
  return a.assetIdentity==b.assetIdentity && a.domainIdentity==b.domainIdentity && a.incarnation==b.incarnation &&
      a.row==b.row && a.domain==b.domain && a.kind==b.kind;
}
std::string resourceLabel(std::string_view name,std::string prefix,uint32_t row) {
  const auto suffix=" · "+std::to_string(row);size_t count=std::min<size_t>(name.size(),192);
  while(count<name.size() && count && (uint8_t(name[count])&0xc0)==0x80) --count;
  if(count) {prefix+=": ";prefix.append(name.substr(0,count));}prefix+=suffix;return prefix;
}
std::string boundedResourceText(std::string_view text,size_t maximum=4096) {
  auto size=std::min(text.size(),maximum);while(size<text.size() && size && (uint8_t(text[size])&0xc0)==0x80) --size;
  return std::string(text.substr(0,size));
}
}

struct RendererFrontend::MaterialBindingBudget {
  static constexpr uint64_t limit=256ull*1024*1024;
  uint64_t reserved{};
};
struct RendererFrontend::MaterialEditHistory {
  ~MaterialEditHistory() {if(budget) budget->reserved-=reservation;}
  std::shared_ptr<MaterialBindingBudget> budget;
  uint64_t reservation{};
  uint64_t graph{},asset{};
  std::shared_ptr<const GltfMaterialAuthoringPlan> command;
  container::material::TextureImageOwnershipPtr images;
  container::scene::TextureResourceReferenceOwnerPtr references;
};
struct RendererFrontend::PendingMaterialGesture {
  uint64_t id{},authoredRevision{};
  int32_t source{};
  std::shared_ptr<MaterialEditHistory> state;
  std::optional<container::ui::MaterialInspectorTarget> uiTarget;
  container::util::EditHistory::Command history;
};
struct RendererFrontend::MaterialPublication {
  std::optional<container::scene::SceneManager::PreparedMaterialGeneration> material;
  std::optional<container::scene::PreparedMaterialBindingChange> bindings;
  std::optional<container::scene::SceneManager::PreparedSourceResourceRetirement> resources;
  int32_t editedSource{-1};
};
struct RendererFrontend::MaterialBindingHistory {
  ~MaterialBindingHistory() {if(budget) budget->reserved-=reservation;}
  std::shared_ptr<MaterialBindingBudget> budget;
  uint64_t graph{},asset{},reservation{};
  container::scene::MaterialBindingCommand bindings;
  container::scene::SceneManager::MaterialAppendCommand appended;
  container::material::TextureImageOwnershipPtr images;
  container::scene::TextureResourceReferenceOwnerPtr references;
};
struct RendererFrontend::MaterialTextureHistory {
  ~MaterialTextureHistory() {if(budget) budget->reserved-=reservation;}
  std::shared_ptr<MaterialBindingBudget> budget;
  uint64_t graph{},asset{},reservation{};
  std::shared_ptr<const GltfMaterialAuthoringPlan> command;
  container::scene::SceneManager::MaterialResourceCommand resources;
  container::material::TextureImageOwnershipPtr images;
  container::scene::TextureResourceReferenceOwnerPtr references;
};
struct RendererFrontend::MaterialRetirementHistory {
  ~MaterialRetirementHistory() {if(budget) budget->reserved-=reservation;}
  std::shared_ptr<MaterialBindingBudget> budget;
  uint64_t graph{},asset{},reservation{};
  container::scene::SceneManager::MaterialRetirementCommand command;
};
struct RendererFrontend::ResourceRetirementHistory {
  ~ResourceRetirementHistory() {if(budget) budget->reserved-=reservation;}
  std::shared_ptr<MaterialBindingBudget> budget;
  uint64_t graph{},asset{},reservation{};
  container::scene::SceneManager::SourceResourceRetirementCommand command;
};
struct RendererFrontend::ResourceCatalogPreview {
  const container::scene::SceneManager* manager{};
  const BimManager* bim{};
  const AnimationPlayer* player{};
  uint64_t asset{},resource{},material{},authored{},geometry{},epoch{},graph{},graphRevision{},objects{};
  std::optional<size_t> clip;
  double time{},speed{};
  bool playing{},loop{},available{},retirementAvailable{};
  std::optional<container::ui::ResourceCatalogTarget> selection;
  std::string error;
  std::weak_ptr<const container::scene::TextureResourceLifecycleGeneration> generation;
  std::weak_ptr<const container::material::TextureImageOwnership> images;
  std::shared_ptr<MaterialExternalProof> external;
  std::vector<uint8_t> materials,textures,imageMask,samplers;
  GltfTextureResourceReferences evaluated;
  std::vector<GltfMaterialRawTextureReferences> rests;
  size_t restCount{};
  bool matches(const RendererFrontend& frontend) const noexcept {
    if(manager!=frontend.subs_.sceneManager.get() || !manager || bim!=frontend.subs_.bimManager.get() ||
        player!=frontend.gltfAnimation_.get() || !frontend.subs_.guiManager || !frontend.subs_.sceneProviderRegistry ||
        asset!=manager->materialAssetIdentity() || resource!=manager->materialResourceRevision() ||
        material!=manager->materialRevision() || authored!=manager->authoredMaterialRevision() || geometry!=manager->geometryRevision() ||
        epoch!=manager->generationEpoch() || graph!=frontend.sceneGraph_.identity() || graphRevision!=frontend.sceneGraph_.revision() ||
        objects!=(frontend.subs_.sceneController?frontend.subs_.sceneController->objectDataRevision():0) ||
        selection!=frontend.resourceCatalogSelection_ || error!=frontend.resourceCatalogError_ ||
        generation.lock()!=manager->resourceLifecycleGeneration() || images.lock()!=manager->textureImageOwnership() ||
        !container::material::TextureImageOwnershipValid(manager->textureImageOwnership(),frontend.svc_.allocationManager) ||
        !external || !external->matches(*frontend.subs_.sceneProviderRegistry)) return false;
    const auto& displayed=frontend.subs_.guiManager->resourceCatalogSnapshot();
    if(displayed.assetIdentity!=asset || displayed.resourceRevision!=resource || displayed.selected!=selection ||
        displayed.available!=available || displayed.retirementAvailable!=retirementAvailable) return false;
    if(player) {
      // Compare the caller's current pointer before inspecting any player data.
      if(player->selectedClip()!=clip || player->time()!=time || player->speed()!=speed || player->playing()!=playing ||
          player->loop()!=loop || player->library().restMaterials.size()!=restCount || rests.size()!=materials.size()) return false;
      for(size_t source=0;source<rests.size();++source) if(materials[source] &&
          !GltfMaterialRawTextureReferencesMatch(player->library().restMaterials[source],
              manager->authoredMaterialResources().textures.size(),{},rests[source])) return false;
    } else if(!rests.empty()) return false;
    return GltfTextureResourceReferencesMatch(manager->animationSource(),{materials,textures,imageMask,samplers},evaluated);
  }
  static std::shared_ptr<ResourceCatalogPreview> capture(const RendererFrontend& frontend,
      const container::ui::ResourceCatalogSnapshot& snapshot,std::shared_ptr<MaterialExternalProof> externalProof) {
    constexpr uint64_t maximum=64ull*1024*1024;uint64_t remaining=maximum;
    const auto charge=[&](uint64_t bytes) {if(bytes>remaining) throw std::length_error("Resource preview witness exceeds its cache budget");remaining-=bytes;};
    if(!snapshot.available || !externalProof || !snapshot.selected || frontend.resourceCatalogError_.size()>8192) return {};
    charge(sizeof(ResourceCatalogPreview)+256);charge(externalProof->ownedBytes());charge(frontend.resourceCatalogError_.size());
    auto result=std::make_shared<ResourceCatalogPreview>();const auto& manager=*frontend.subs_.sceneManager;
    result->manager=&manager;result->bim=frontend.subs_.bimManager.get();result->player=frontend.gltfAnimation_.get();
    result->asset=manager.materialAssetIdentity();result->resource=manager.materialResourceRevision();
    result->material=manager.materialRevision();result->authored=manager.authoredMaterialRevision();result->geometry=manager.geometryRevision();
    result->epoch=manager.generationEpoch();result->graph=frontend.sceneGraph_.identity();result->graphRevision=frontend.sceneGraph_.revision();
    result->objects=frontend.subs_.sceneController?frontend.subs_.sceneController->objectDataRevision():0;
    result->selection=snapshot.selected;result->error=frontend.resourceCatalogError_;result->available=snapshot.available;
    result->retirementAvailable=snapshot.retirementAvailable;result->external=std::move(externalProof);
    const auto generation=manager.resourceLifecycleGeneration();result->generation=generation;result->images=manager.textureImageOwnership();
    if(!generation || snapshot.assetIdentity!=result->asset || snapshot.resourceRevision!=result->resource) return {};
    const auto& source=manager.animationSource();const auto mapping=manager.sourceMaterialRuntimeMapping();
    if(mapping.size()!=source.materials.size() || mapping.size()>1'000'000) return {};
    charge(mapping.size());result->materials.reserve(mapping.size());
    for(const auto runtime:mapping) result->materials.push_back(manager.materialIsActive(runtime)?1:0);
    const auto phase=[&](container::util::ResourceKind kind,size_t count,std::vector<uint8_t>& output) {
      if(count>1'000'000) throw std::length_error("Resource preview source table exceeds its limit");
      charge(count);output.reserve(count);
      for(uint32_t row=0;row<count;++row) output.push_back(generation->active({container::util::ResourceDomainKind::Evaluated,kind,row})?1:0);
    };
    phase(container::util::ResourceKind::Texture,source.textures.size(),result->textures);
    phase(container::util::ResourceKind::Image,source.images.size(),result->imageMask);
    phase(container::util::ResourceKind::Sampler,source.samplers.size(),result->samplers);
    GltfTextureResourceReferenceLimits limits;limits.maxOwnedBytes=remaining;
    limits.policy=snapshot.retirementAvailable?GltfTextureResourceReferencePolicy::RequireComplete:GltfTextureResourceReferencePolicy::RecordIncomplete;
    result->evaluated=ExtractGltfTextureResourceReferences(source,{result->materials,result->textures,result->imageMask,result->samplers},limits);
    charge(result->evaluated.stats().ownedBytes);
    if(result->player) {
      result->clip=result->player->selectedClip();result->time=result->player->time();result->speed=result->player->speed();
      result->playing=result->player->playing();result->loop=result->player->loop();result->restCount=result->player->library().restMaterials.size();
      if(result->restCount<mapping.size()) return {};
      charge(mapping.size()*sizeof(GltfMaterialRawTextureReferences));result->rests.resize(mapping.size());
      for(size_t row=0;row<mapping.size();++row) if(result->materials[row]) {
        limits.maxOwnedBytes=remaining;result->rests[row]=ExtractGltfMaterialRawTextureReferences(result->player->library().restMaterials[row],
            manager.authoredMaterialResources().textures.size(),{},limits);charge(result->rests[row].stats().ownedBytes);
      }
    }
    return result;
  }
};

bool RendererFrontend::publishMaterialEdit(const GltfMaterialAuthoringPlan& plan) {
  if(!plan.changed) return true;
  if(!subs_.sceneManager) throw std::runtime_error("Material editing requires an initialized scene renderer");
  MaterialPublication publication;
  publication.material.emplace(subs_.sceneManager->prepareMaterialGeneration(plan,gltfAnimation_.get()));
  publication.editedSource=plan.destinationMaterial;
  return publishMaterialTransaction(publication);
}

bool RendererFrontend::authorMaterialTexture(const container::material::GltfMaterialTextureEditRequest& request) {
  finishLightEditInput();
  using Operation=container::material::MaterialTextureEditOperation;
  using Manager=container::scene::SceneManager;
  container::material::ValidateMaterialTextureEditRequest(request);
  if(!subs_.sceneManager) throw std::runtime_error("Texture editing requires an initialized scene renderer");
  auto& manager=*subs_.sceneManager;
  if(request.expectedResourceRevision && *request.expectedResourceRevision!=manager.materialResourceRevision())
    throw std::invalid_argument("The texture catalog changed; refresh before editing");
  const auto mapping=manager.sourceMaterialRuntimeMapping();
  if(size_t(request.sourceMaterial)>=mapping.size() || size_t(request.sourceMaterial)>=manager.animationSource().materials.size())
    throw std::invalid_argument("The selected material no longer exists");
  const auto runtime=mapping[size_t(request.sourceMaterial)];
  if(!manager.materialIsActive(runtime) || !manager.authoredMaterialIsActive(request.sourceMaterial))
    throw std::invalid_argument("The selected material is retired");
  if(manager.isLinkedMaterialReadOnly(runtime))
    throw std::invalid_argument("This linked asset is read-only. Use Make Editable first.");
  const auto& authored=manager.authoredMaterial(request.sourceMaterial);
  container::scene::MaterialGenerationLimits limits;
  (void)GltfMaterialRecordStorageBytes(authored,limits.authoring);
  (void)GltfMaterialRecordStorageBytes(manager.animationSource().materials[size_t(request.sourceMaterial)],limits.authoring);
  const auto binding=ReadGltfMaterialTextureBinding(authored,request.slot);
  if((request.operation==Operation::EditMapping || request.operation==Operation::EditSampler) && binding.index<0)
    throw std::invalid_argument("Bind a texture before editing its settings");
  if(pendingSceneRetirement_ || sceneEditPublicationPending_) publishSceneEdit();
  if(request.expectedResourceRevision && *request.expectedResourceRevision!=manager.materialResourceRevision())
    throw std::invalid_argument("The texture catalog changed before preparation");
  ensureSceneEditHistory();
  const auto& info=GltfMaterialTextureSlotDescription(request.slot);
  const bool colorResource=info.srgb || binding.coupledSlot.has_value();
  const auto roles=colorResource?container::scene::MaterialTextureRoles::Color:container::scene::MaterialTextureRoles::Data;
  container::scene::MaterialResourcesLimits resourceLimits;
  resourceLimits.maxPreparedCpuBytes=std::min(resourceLimits.maxPreparedCpuBytes,limits.maxPreparedCpuBytes);
  std::optional<Manager::PreparedMaterialResources> resources;
  if(request.operation==Operation::ImportFile) {
    const auto sampler=request.sampler?*request.sampler:
        materialTextureSampler(manager.authoredMaterialResources(),static_cast<uint32_t>(binding.index));
    resources.emplace(manager.prepareMaterialTextureFileWithAuthoredSampler(request.sourceMaterial,container::util::pathFromUtf8(*request.path),
        roles,sampler,resourceLimits));
  } else if(request.operation==Operation::BindExisting || request.operation==Operation::EditSampler) {
    const auto texture=request.operation==Operation::BindExisting?*request.sourceTexture:uint32_t(binding.index);
    if(request.sampler) resources.emplace(manager.prepareMaterialTextureSourceWithAuthoredSampler(request.sourceMaterial,
        texture,roles,*request.sampler,resourceLimits));
    else resources.emplace(manager.prepareMaterialTextureSource(request.sourceMaterial,
        texture,roles,std::nullopt,resourceLimits));
  }
  GltfMaterialTextureEdit edit;edit.slot=request.slot;edit.mapping=request.mapping;
  if(request.operation==Operation::Clear) edit.operation=GltfMaterialTextureOperation::Clear;
  else if(request.operation==Operation::EditMapping) edit.operation=GltfMaterialTextureOperation::Mapping;
  else {
    edit.operation=GltfMaterialTextureOperation::Bind;
    const auto reference=resources->reference(colorResource);
    if(!reference.evaluatedTexture()) throw std::invalid_argument("The texture has no evaluated source identity");
    edit.binding=GltfMaterialTextureSourcePair{int32_t(reference.archiveTexture()),int32_t(*reference.evaluatedTexture())};
  }
  const auto resourceBytes=resources?resources->ownedBytes():0;
  if(resourceBytes>=limits.maxPreparedCpuBytes) throw std::length_error("Texture editing exceeds its aggregate preparation budget");
  limits.authoring.maxOutputBytes=std::min(limits.authoring.maxOutputBytes,(limits.maxPreparedCpuBytes-resourceBytes)/3);
  if(!limits.authoring.maxOutputBytes) throw std::length_error("Texture editing has no remaining material plan budget");
  GltfMaterialPatch patch;patch.textures.push_back(std::move(edit));
  const auto plan=manager.planMaterialTextureEdit(request.sourceMaterial,patch,resources?&*resources:nullptr,limits.authoring);
  if(!plan.changed) {
    sceneEdits_->finishGesture();finishMaterialGesture();
    materialTextureError_.clear();materialTextureErrorSource_=-1;materialTextureErrorAsset_=0;
    return true;
  }
  auto state=std::make_shared<MaterialTextureHistory>();state->graph=sceneGraph_.identity();state->asset=manager.materialAssetIdentity();
  state->command=std::make_shared<const GltfMaterialAuthoringPlan>(plan);
  if(resources) state->resources=resources->command();
  if(!materialBindingBudget_) materialBindingBudget_=std::make_shared<MaterialBindingBudget>();
  state->images=manager.retainMaterialPlanImages(plan,resources?&*resources:nullptr,
      MaterialBindingBudget::limit-materialBindingBudget_->reserved);
  const auto available=MaterialBindingBudget::limit-materialBindingBudget_->reserved;
  const auto beforeReferences=uint64_t(sizeof(MaterialTextureHistory))+plan.peakOwnedBytes+state->resources.ownedBytes()+state->images->ownedBytes();
  if(beforeReferences>=available) throw std::length_error("Retained material history has no logical resource budget");
  state->references=manager.retainMaterialPlanResourceReferences(plan,resources?&*resources:nullptr,available-beforeReferences);
  const auto bytes=beforeReferences+state->references->ownedBytes();
  if(bytes>MaterialBindingBudget::limit-materialBindingBudget_->reserved)
    throw std::length_error("Retained material history exceeds its resource budget");
  materialBindingBudget_->reserved+=bytes;state->budget=materialBindingBudget_;state->reservation=bytes;
  container::util::EditHistory::Command history{"Edit material texture",
      [this,state]{return replayMaterialTexture(*state,false);},[this,state]{return replayMaterialTexture(*state,true);},true};
  MaterialPublication publication;publication.editedSource=request.sourceMaterial;
  if(resources) publication.material.emplace(manager.prepareMaterialGeneration(plan,gltfAnimation_.get(),std::move(*resources),limits));
  else publication.material.emplace(manager.prepareMaterialGeneration(plan,gltfAnimation_.get(),limits));
  validateTextureFrameReuse(manager,sceneGraph_,runtime,publication.material->evaluatedMaterial());
  if(!publishMaterialTransaction(publication)) return false;
  // Keep a preceding gesture and redo cursor intact until every resource and
  // renderer stage has succeeded. The command and captures already exist.
  sceneEdits_->finishGesture();finishMaterialGesture();
  sceneEdits_->recordAppliedPublished(std::move(history));
  materialTextureError_.clear();materialTextureErrorSource_=-1;materialTextureErrorAsset_=0;
  return true;
}

bool RendererFrontend::replayMaterialTexture(const MaterialTextureHistory& state,bool forward) {
  if(!subs_.sceneManager || sceneGraph_.identity()!=state.graph || subs_.sceneManager->materialAssetIdentity()!=state.asset) return false;
  if(!container::material::TextureImageOwnershipValid(state.images,svc_.allocationManager)) return false;
  if(!state.references || !state.references->valid() || (!state.resources &&
      (!state.references->resourceDependenciesActive() || !state.references->resourceDependenciesMatch(subs_.sceneManager->resourceLifecycleGeneration())))) return false;
  auto& manager=*subs_.sceneManager;
  const auto mapping=manager.sourceMaterialRuntimeMapping();
  const auto source=state.command->destinationMaterial;
  if(source<0 || size_t(source)>=mapping.size() || manager.isLinkedMaterialReadOnly(mapping[size_t(source)])) return false;
  std::optional<container::scene::SceneManager::PreparedMaterialResources> resources;
  if(state.resources) resources.emplace(manager.prepareMaterialResourcesChange(state.resources,forward));
  const auto plan=manager.rebaseMaterialTextureEdit(*state.command,
      forward?GltfMaterialEditSide::After:GltfMaterialEditSide::Before,resources?&*resources:nullptr);
  MaterialPublication publication;publication.editedSource=plan.destinationMaterial;
  if(resources) publication.material.emplace(manager.prepareMaterialGeneration(plan,gltfAnimation_.get(),std::move(*resources)));
  else publication.material.emplace(manager.prepareMaterialGeneration(plan,gltfAnimation_.get()));
  validateTextureFrameReuse(manager,sceneGraph_,publication.material->runtimeMaterial(),publication.material->evaluatedMaterial());
  return publishMaterialTransaction(publication);
}

bool RendererFrontend::authorMaterialRetirement(uint32_t runtime,int32_t source,std::optional<uint64_t> expectedRevision) {
  finishLightEditInput();
  if(!subs_.sceneManager) throw std::runtime_error("Material deletion requires an initialized scene renderer");
  auto& manager=*subs_.sceneManager;
  if(source< -1 || !manager.materialIsActive(runtime) || manager.sourceMaterialForRuntime(runtime)!=source ||
      (source>=0 && !manager.authoredMaterialIsActive(source)))
    throw std::invalid_argument("The material catalog identity is stale or retired");
  if(manager.isLinkedMaterialReadOnly(runtime))
    throw std::invalid_argument("This linked asset is read-only. Use Make Editable first.");
  if(expectedRevision && *expectedRevision!=manager.authoredMaterialRevision())
    throw std::invalid_argument("The material catalog changed; refresh before deleting");
  if(pendingSceneRetirement_ || sceneEditPublicationPending_) publishSceneEdit();
  if(expectedRevision && *expectedRevision!=manager.authoredMaterialRevision())
    throw std::invalid_argument("The material changed before deletion preparation");
  ensureSceneEditHistory();
  if(!materialBindingBudget_) materialBindingBudget_=std::make_shared<MaterialBindingBudget>();
  auto budget=materialBindingBudget_;
  if(budget->reserved>=MaterialBindingBudget::limit) throw std::length_error("Material history budget is exhausted");
  const auto available=MaterialBindingBudget::limit-budget->reserved;
  container::scene::MaterialGenerationLimits limits;
  MaterialPublication publication;publication.editedSource=source;
  publication.material.emplace(manager.prepareMaterialRetirement(runtime,sceneGraph_,gltfAnimation_.get(),limits));
  auto state=std::make_shared<MaterialRetirementHistory>();state->graph=sceneGraph_.identity();state->asset=manager.materialAssetIdentity();
  state->command=publication.material->retirementCommand();
  if(!state->command || state->command.runtimeMaterial()!=runtime || state->command.sourceMaterial()!=source)
    throw std::runtime_error("Material deletion did not issue the selected retirement receipt");
  constexpr uint64_t overhead=sizeof(MaterialRetirementHistory)+1024;
  const auto bytes=state->command.ownedBytes();
  if(bytes>available || overhead>available-bytes) throw std::length_error("Material retirement history exceeds its budget");
  state->reservation=bytes+overhead;state->budget=budget;budget->reserved+=state->reservation;
  container::util::EditHistory::Command history{"Delete unused material",[this,state]{return replayMaterialRetirement(*state,false);},
      [this,state]{return replayMaterialRetirement(*state,true);},true};
  if(!publishMaterialTransaction(publication)) return false;
  sceneEdits_->finishGesture();finishMaterialGesture();sceneEdits_->recordAppliedPublished(std::move(history));
  materialCatalogError_.clear();return true;
}
bool RendererFrontend::replayMaterialRetirement(const MaterialRetirementHistory& state,bool retired) {
  if(!subs_.sceneManager || sceneGraph_.identity()!=state.graph || subs_.sceneManager->materialAssetIdentity()!=state.asset) return false;
  auto& manager=*subs_.sceneManager;MaterialPublication publication;publication.editedSource=state.command.sourceMaterial();
  if(manager.isLinkedMaterialReadOnly(state.command.runtimeMaterial())) return false;
  publication.material.emplace(manager.prepareMaterialRetirementChange(state.command,
      retired?container::scene::MaterialRetirementState::Retired:container::scene::MaterialRetirementState::Active,
      sceneGraph_,gltfAnimation_.get()));
  if(!publishMaterialTransaction(publication)) return false;
  materialCatalogError_.clear();return true;
}

bool RendererFrontend::authorResourceRetirement(const container::ui::ResourceRetirementRequest& request) {
  finishLightEditInput();
  if(!subs_.sceneManager) throw std::invalid_argument("Resource deletion requires an initialized scene");
  auto& manager=*subs_.sceneManager;(void)resolveResourceTarget(manager,request.target);
  const auto selection=resourceSelection(request.target);
  if(manager.isLinkedResourceReadOnly(selection)) throw std::invalid_argument("This linked asset is read-only. Use Make Editable first.");
  if(pendingSceneRetirement_ || sceneEditPublicationPending_) publishSceneEdit();
  (void)resolveResourceTarget(manager,request.target);
  ensureSceneEditHistory();
  if(!materialBindingBudget_) materialBindingBudget_=std::make_shared<MaterialBindingBudget>();
  const auto budget=materialBindingBudget_;
  if(budget->reserved>=MaterialBindingBudget::limit) throw std::length_error("Resource Undo history is full");
  const auto available=MaterialBindingBudget::limit-budget->reserved;
  // Providers consume Material IDs. Their exact active incarnations must be
  // proved before orphan planning and again during renderer publication.
  if(!subs_.sceneProviderRegistry) throw std::invalid_argument("The resource provider registry is unavailable");
  MaterialExternalProof external(subs_.bimManager.get(),*subs_.sceneProviderRegistry,manager.materialCount(),false,std::nullopt,&manager);
  const auto selections=manager.planResourceRetirement(selection,gltfAnimation_.get(),
      std::min<uint64_t>(64ull*1024*1024,available));
  (void)resolveResourceTarget(manager,request.target);
  if(selections.empty()) throw std::invalid_argument("No current unused resources were selected");
  if(std::ranges::any_of(selections,[&](const auto& resource) {return manager.isLinkedResourceReadOnly(resource);}))
    throw std::invalid_argument("Resource deletion includes read-only linked asset resources");
  MaterialPublication publication;publication.resources.emplace(manager.prepareSourceResourceRetirement(
      selections,gltfAnimation_.get(),available));
  auto state=std::make_shared<ResourceRetirementHistory>();state->graph=sceneGraph_.identity();state->asset=manager.materialAssetIdentity();
  state->command=publication.resources->command();
  if(!state->command || !external.matches(*subs_.sceneProviderRegistry))
    throw std::invalid_argument("Resource consumers changed while preparing deletion");
  constexpr uint64_t overhead=sizeof(ResourceRetirementHistory)+1024;
  const auto bytes=state->command.ownedBytes();
  if(bytes>available || overhead>available-bytes) throw std::length_error("Selected resource Undo history exceeds its budget");
  state->reservation=bytes+overhead;state->budget=budget;budget->reserved+=state->reservation;
  container::util::EditHistory::Command history{"Delete unused resource",[this,state]{return replayResourceRetirement(*state,false);},
      [this,state]{return replayResourceRetirement(*state,true);},true};
  if(!publishMaterialTransaction(publication)) return false;
  sceneEdits_->finishGesture();finishMaterialGesture();sceneEdits_->recordAppliedPublished(std::move(history));
  resourceCatalogError_.clear();return true;
}

bool RendererFrontend::replayResourceRetirement(const ResourceRetirementHistory& state,bool retired) {
  if(!subs_.sceneManager || sceneGraph_.identity()!=state.graph || subs_.sceneManager->materialAssetIdentity()!=state.asset || !state.command)
    return false;
  using Phase=container::util::ResourceLifecyclePhase;
  if(std::ranges::any_of(state.command.selections(),[&](const auto& resource) {
      return subs_.sceneManager->isLinkedResourceReadOnly(resource);})) return false;
  const auto available=state.budget?MaterialBindingBudget::limit-state.budget->reserved:MaterialBindingBudget::limit;
  if(!available) throw std::length_error("Resource replay has no remaining preparation budget");
  MaterialPublication publication;publication.resources.emplace(subs_.sceneManager->prepareSourceResourceRetirement(
      state.command,retired?Phase::Retired:Phase::Active,gltfAnimation_.get(),available));
  if(!publishMaterialTransaction(publication)) return false;
  resourceCatalogError_.clear();return true;
}

bool RendererFrontend::publishMaterialTransaction(MaterialPublication& publication) {
  if(!subs_.sceneManager || !subs_.sceneController || !subs_.lightingManager || !subs_.frameResourceManager ||
      !subs_.frameRuntimeResourceRegistry || !subs_.sceneProviderSynchronizer || !subs_.sceneProviderRegistry)
    throw std::runtime_error("Material editing requires an initialized scene renderer");
  if(materialPublications_==UINT64_MAX) throw std::overflow_error("Material publication counter exhausted");
  auto& manager=*subs_.sceneManager;auto& controller=*subs_.sceneController;auto& lighting=*subs_.lightingManager;
  const auto imageCount=uint32_t(svc_.swapChainManager.imageCount());
  if(!imageCount || buffers_.cameras.size()!=imageCount || buffers_.objects.size()!=imageCount ||
      buffers_.objectCapacities.size()!=imageCount || subs_.frameResourceManager->frameCount()!=imageCount)
    throw std::runtime_error("Material editing frame resources are not ready");
  if(subs_.ddgiManager) subs_.ddgiManager->synchronizeAsync();
  if(vkDeviceWaitIdle(svc_.ctx.deviceWrapper->device())!=VK_SUCCESS)
    throw std::runtime_error("Failed to retire material readers");
  const bool showDiagCube=subs_.guiManager && subs_.guiManager->showNormalDiagCube();
  if(!controller.materialRoutingCacheCurrent(showDiagCube))
    throw std::runtime_error("Scene object state changed; finish its publication before editing materials");
  const auto graphIdentity=sceneGraph_.identity(),graphRevision=sceneGraph_.revision();
  const auto epoch=manager.generationEpoch(),geometry=controller.geometryRevision(),objectRevision=controller.objectDataRevision();
  auto& material=publication.material;auto& bindings=publication.bindings;auto& resources=publication.resources;
  if((!material && !bindings && !resources) || (resources && (material || bindings)))
    throw std::invalid_argument("Material/resource publication has an invalid candidate combination");
  std::vector<container::scene::MaterialRenderProperties> residentProperties;
  if(!material) {
    if(manager.materialCount()>1'000'000) throw std::length_error("Material routing table exceeds its limit");
    residentProperties.reserve(manager.materialCount());
    for(uint32_t i=0;i<manager.materialCount();++i) residentProperties.push_back(manager.materialRenderProperties(i));
  }
  const auto facts=material?material->materialProperties():std::span<const container::scene::MaterialRenderProperties>(residentProperties);
  const auto materialRevision=material?material->materialRevision():manager.materialRevision();
  const bool extended=material?material->hasExtendedPbrMaterials():manager.hasExtendedPbrMaterials();
  MaterialExternalProof external(subs_.bimManager.get(),*subs_.sceneProviderRegistry,facts.size(),facts.size()<manager.materialCount(),
      material?material->retiringMaterial():std::nullopt,resources?&manager:nullptr);
  if(resources && !resources->runtimeChanged()) {
    // Source phases and export/cache authority can change without touching a
    // sampled runtime row. Keep the existing descriptor/ray/lighting histories
    // when the candidate proves that its actual sampling resources are equal.
    if(subs_.ddgiManager) subs_.ddgiManager->synchronizeAsync();
    if(vkDeviceWaitIdle(svc_.ctx.deviceWrapper->device())!=VK_SUCCESS)
      throw std::runtime_error("Failed to retire source resource readers");
    if(sceneGraph_.identity()!=graphIdentity || sceneGraph_.revision()!=graphRevision ||
        controller.geometryRevision()!=geometry || controller.objectDataRevision()!=objectRevision ||
        pendingSceneRetirement_ || sceneEditPublicationPending_ || !external.matches(*subs_.sceneProviderRegistry) ||
        !manager.canExchangeSourceResourceRetirement(*resources,gltfAnimation_.get()))
      throw std::runtime_error("Scene changed while preparing a source resource edit");
    manager.exchangeSourceResourceRetirement(*resources,gltfAnimation_.get());
    lastEditedSourceMaterial_=-1;++materialPublications_;return true;
  }
  std::optional<SceneController::PreparedMaterialRouting> routing;
  if(bindings) routing=controller.prepareMaterialRouting(facts,*bindings,imageCount,showDiagCube,epoch);
  else if(material && material->routingChanged()) routing=controller.prepareMaterialRouting(facts,imageCount,showDiagCube,epoch);
  const auto objects=routing?routing->objectBuffers():std::span<const container::gpu::AllocatedBuffer>(buffers_.objects);
  const auto capacities=routing?routing->objectBufferCapacities():std::span<const size_t>(buffers_.objectCapacities);
  const auto objectData=routing?routing->objectData():std::span<const container::gpu::ObjectData>(controller.objectData());
  const auto opaque=routing?routing->opaqueDrawCommands():std::span<const DrawCommand>(controller.opaqueDrawCommands());
  const auto transparent=routing?routing->transparentDrawCommands():std::span<const DrawCommand>(controller.transparentDrawCommands());
  const auto singleSided=routing?routing->opaqueSingleSidedDrawCommands():std::span<const DrawCommand>(controller.opaqueSingleSidedDrawCommands());
  const auto futureObject=routing?routing->objectDataRevision():objectRevision;
  for(const auto& object:objectData) if(object.objectInfo.x>=facts.size())
    throw std::invalid_argument("A prospective object references an absent material");
  std::optional<container::scene::SceneManager::PreparedSceneBindings> sceneBindings;
  if(!material) {
    if(resources) sceneBindings.emplace(manager.prepareSceneBindings(*resources,{},gltfAnimation_.get()));
    else sceneBindings.emplace(manager.prepareSceneBindings());
  }
  const auto prepareDescriptors=[&](std::span<const container::gpu::AllocatedBuffer> auxiliary={}) {
    if(material) material->prepareDescriptorFrames(buffers_.cameras,objects,auxiliary);
    else sceneBindings->prepareDescriptorFrames(buffers_.cameras,objects,auxiliary,resources?gltfAnimation_.get():nullptr);
  };
  const auto descriptorSet=[&](uint32_t image) {return material?material->descriptorSet(image):sceneBindings->descriptorSet(image);};
  const auto auxiliaryDescriptorSet=[&](uint32_t image) {return material?material->auxiliaryDescriptorSet(image):sceneBindings->auxiliaryDescriptorSet(image);};
  if(subs_.bimManager && subs_.bimManager->hasScene()) {
    const auto& bimObject=subs_.bimManager->objectAllocatedBuffer();
    prepareDescriptors(std::span(&bimObject,1));
  } else prepareDescriptors();

  std::unique_ptr<PbrAuxiliaryPass> auxiliary;
  auto* auxiliaryPass=subs_.pbrAuxiliaryPass.get();
  if(extended && (!auxiliaryPass || !auxiliaryPass->ready())) {
    std::vector<VkImageView> depths;depths.reserve(imageCount);
    for(uint32_t i=0;i<imageCount;++i) {
      const auto* frame=subs_.frameResourceManager->frame(i);
      depths.push_back(msaaSampleCount_==VK_SAMPLE_COUNT_1_BIT?frame->depthStencil.view:frame->depthStencilMsaa.view);
    }
    auxiliary=std::make_unique<PbrAuxiliaryPass>(svc_.ctx.deviceWrapper,svc_.allocationManager,svc_.pipelineManager);
    auxiliary->createResources(activeShaderReload_ ? activeShaderReload_->directory : container::util::executableDirectory(),
        manager.descriptorSetLayout(),resources_.gBufferFormats.depthStencil,
        msaaSampleCount_,depths,svc_.swapChainManager.extent());auxiliaryPass=auxiliary.get();
  }
  std::vector<VkImageView> anisotropy,diffuse;
  if(auxiliaryPass && auxiliaryPass->ready()) for(uint32_t i=0;i<imageCount;++i) {
    anisotropy.push_back(auxiliaryPass->anisotropyView(i));diffuse.push_back(auxiliaryPass->diffuseTransmissionView(i));
  }
  auto auxiliaryBindings=subs_.frameResourceManager->prepareAuxiliaryBindings(anisotropy,diffuse);
  std::optional<GpuCullManager::PreparedGeneration> culling;
  if(routing && subs_.gpuCullManager) culling=subs_.gpuCullManager->prepareGeneration(objects,capacities,buffers_.cameras,
      sizeof(container::gpu::CameraData),singleSided,futureObject);
  std::optional<ShadowCullManager::PreparedGeneration> shadowCulling;
  if(routing && subs_.shadowCullManager && subs_.shadowManager) {
    std::vector<VkBuffer> ubos;for(const auto& buffer:subs_.shadowManager->shadowCullUbos()) ubos.push_back(buffer.buffer);
    shadowCulling=subs_.shadowCullManager->prepareGeneration(objects,capacities,ubos,sizeof(container::gpu::ShadowCullData),
        singleSided,materialShadowDrawRevision(singleSided));
  }
  const auto& bounds=manager.modelBounds();
  container::scene::SceneProviderBounds meshBounds{bounds.min,bounds.max,bounds.valid};
  if(manager.deformationData()) {const auto b=controller.nodeWorldBounds(sceneState_.rootNode);if(b.valid) meshBounds={b.min,b.max,true};}
  container::scene::MeshSceneProviderAssetCache assetCache;
  const auto& asset=assetCache.get({.primitiveRanges=manager.primitiveRanges(),.materialCount=facts.size(),
      .instanceCount=objectData.size(),.bounds=meshBounds,.materialFactsAt=[&](uint32_t id) {
        const auto& value=facts[id];return container::scene::MeshSceneProviderMaterialFacts{value.transparent,value.doubleSided};
      },.bindings=MakeMeshSceneProviderBindingView(objectData,opaque,transparent,uint64_t(controller.geometryIndices().size()))},
      {.geometry=manager.geometryRevision(),.materials=materialRevision,.instances=futureObject},&manager);
  MeshSceneProviderSyncInput mesh{.available=asset.primitiveCount>0 || asset.bounds.valid,.primitiveCount=asset.primitiveCount,
      .materialCount=asset.materialCount,.instanceCount=asset.instanceCount,.triangleBatches=asset.triangleBatches,.bounds=asset.bounds,
      .geometryRevision=geometry,.materialRevision=materialRevision,.instanceRevision=futureObject,
      .displayName=activePrimaryModelPath_.empty()?"Primary mesh scene":activePrimaryModelPath_,
      .triangleBatchesAuthoritative=asset.triangleBatchesAuthoritative};
  const auto providerInput=buildSceneProviderSyncInput(&mesh);
  SceneProviderSynchronizer providers;container::scene::SceneProviderRegistry registry;providers.sync(registry,providerInput);

  std::unique_ptr<RayShadowManager> ray;
  std::optional<LightingManager::PreparedRayShadowBindings> lightBindings;
  if(subs_.rayShadowManager) {
    lightBindings=lighting.prepareRayShadowBindings(imageCount);
    ray=subs_.rayShadowManager->createDetachedGeneration();
    const bool gi=subs_.ddgiManager && subs_.ddgiManager->wantsScene() && pushConstants_.bindless.semanticColorMode==0 &&
        (!sceneGraph_.renderableNodes().empty() || (subs_.bimManager && subs_.bimManager->hasScene()));
    ray->requestSurfaceScene(gi,subs_.ddgiManager?subs_.ddgiManager->settings().surfaceBudgetBytes:svc_.config.ddgi.surfaceBudgetBytes);
    const bool wants=ray->supported() && (gi || ray->settings().mode!=RayShadowMode::Raster);
    std::vector<DrawCommand> meshDraws(opaque.begin(),opaque.end());if(gi) meshDraws.insert(meshDraws.end(),transparent.begin(),transparent.end());
    std::vector<RaySceneProviderSource> sources;
    if(mesh.available && wants) sources.push_back({{"primary-mesh-scene"},geometry,controller.geometryVertices(),controller.geometryIndices(),objectData,meshDraws});
    std::vector<DrawCommand> bimDraws;
    if(subs_.bimManager && subs_.bimManager->hasScene() && wants) {
      auto filter=currentBimDrawFilter();filter.drawBudgetEnabled=false;filter.drawBudgetMaxObjects=0;
      const auto add=[&](const auto& draws){for(const auto& draw:draws) if(subs_.bimManager->objectMatchesFilter(draw.objectIndex,filter) &&
          bimObjectVisibleByLayer(draw.objectIndex)) bimDraws.push_back(draw);};
      add(subs_.bimManager->opaqueDrawCommands());if(gi) add(subs_.bimManager->transparentDrawCommands());
      sources.push_back({{"auxiliary-bim-scene"},subs_.bimManager->geometryRevision(),subs_.bimManager->vertices(),
          subs_.bimManager->indices(),subs_.bimManager->objectData(),bimDraws});
    }
    bool compatible=true;
    for(const auto& source:sources) for(const auto& draw:source.draws) for(uint32_t i=0;i<draw.instanceCount;++i) {
      const auto object=uint64_t(draw.objectIndex)+i;
      if(object>=source.objects.size() || !(material?material->rayGeometryCompatible(source.objects[size_t(object)].objectInfo.x):
          manager.rayGeometryCompatible(source.objects[size_t(object)].objectInfo.x))) compatible=false;
    }
    for(uint32_t i=0;i<imageCount;++i) {
      ray->update(i,buffers_.cameraData,*subs_.frameResourceManager->frame(i),descriptorSet(i),lightBindings->lightDescriptorSet(i),
          sources,lighting.lightingData(),lighting.pointLightsSsbo(),lighting.areaLightsSsbo(),pushConstants_.bindless.sectionPlaneEnabled,
          pushConstants_.bindless.sectionPlane,manager.sceneClipState(),[&](uint32_t id){return material?material->temporalMaterialRevision(id):manager.temporalMaterialRevision(id);},compatible);
      svc_.allocationManager.executeStagedCommands([&](VkCommandBuffer cmd){ray->recordBuild(cmd,i);});
      lightBindings->updateRayShadowDescriptors(i,ray->readSettings(i),ray->readSettingsSize(),ray->visibilityView());
    }
  }
  std::optional<DdgiManager::PreparedSceneHistoryReset> ddgi;
  if(subs_.ddgiManager) ddgi=subs_.ddgiManager->prepareSceneHistoryReset();
  auto runtime=*subs_.frameRuntimeResourceRegistry;
  for(uint32_t i=0;i<imageCount;++i) for(const auto technique:{RenderTechniqueId::DeferredRaster,RenderTechniqueId::ForwardRaster}) {
    runtime.bindDescriptorSet(technique,"scene-descriptor-set",i,{descriptorSet(i)});
    runtime.bindDescriptorSet(technique,"light-descriptor-set",i,{lightBindings?lightBindings->lightDescriptorSet(i):lighting.lightDescriptorSet(i)});
    if(subs_.bimManager && subs_.bimManager->hasScene()) runtime.bindDescriptorSet(technique,"bim-scene-descriptor-set",i,{auxiliaryDescriptorSet(i)});
    runtime.bindBuffer(technique,"scene-object-buffer",i,{objects[i].buffer,sizeof(container::gpu::ObjectData)*capacities[i],VK_BUFFER_USAGE_STORAGE_BUFFER_BIT});
  }
  std::vector<container::gpu::AllocatedBuffer> nextObjects(objects.begin(),objects.end());
  std::vector<size_t> nextCapacities(capacities.begin(),capacities.end());
  std::vector<bool> shadowReady(imageCount,shadowCulling.has_value());std::string reason=resources?"resource retirement":"material edit";
  if(subs_.ddgiManager) subs_.ddgiManager->synchronizeAsync();
  if(vkDeviceWaitIdle(svc_.ctx.deviceWrapper->device())!=VK_SUCCESS) throw std::runtime_error("Failed to retire staged material readers");
  if(sceneGraph_.identity()!=graphIdentity || sceneGraph_.revision()!=graphRevision || controller.geometryRevision()!=geometry ||
      controller.objectDataRevision()!=objectRevision ||
      !external.matches(*subs_.sceneProviderRegistry) ||
      (material && !manager.canExchangeMaterialGeneration(*material,gltfAnimation_.get())) ||
      (resources && !manager.canExchangeSourceResourceRetirement(*resources,gltfAnimation_.get())) ||
      (sceneBindings && !manager.canExchangeSceneBindings(*sceneBindings,resources?gltfAnimation_.get():nullptr)) ||
      (bindings && !container::scene::CanPublishMaterialBindings(sceneGraph_,*bindings,materialBindingContext(manager))) ||
      (routing && !controller.canExchangeMaterialRouting(*routing,epoch)) ||
      (culling && !subs_.gpuCullManager->canExchangeGeneration(*culling)) ||
      (shadowCulling && !subs_.shadowCullManager->canExchangeGeneration(*shadowCulling)) ||
      (ray && !subs_.rayShadowManager->canExchangeGeneration(*ray)) ||
      (lightBindings && !lighting.canExchangeRayShadowBindings(*lightBindings)) ||
      !subs_.frameResourceManager->canPublishAuxiliaryBindings(auxiliaryBindings) ||
      (ddgi && !subs_.ddgiManager->canPublishSceneHistoryReset(*ddgi)))
    throw std::runtime_error("Scene changed while preparing a material edit");
  if(bindings) {const bool ok=container::scene::PublishMaterialBindings(sceneGraph_,*bindings,materialBindingContext(manager));assert(ok);}
  if(material) manager.exchangeMaterialGeneration(*material,gltfAnimation_.get());
  else {
    if(resources) manager.exchangeSourceResourceRetirement(*resources,gltfAnimation_.get());
    manager.exchangeSceneBindings(*sceneBindings,resources?gltfAnimation_.get():nullptr);
  }
  if(routing) controller.exchangeMaterialRouting(*routing,manager.materialRevision(),manager.generationEpoch());
  else controller.acknowledgeMaterialRevision(manager.materialRevision());
  if(culling){const bool ok=subs_.gpuCullManager->exchangeGeneration(*culling);assert(ok);}
  if(shadowCulling){const bool ok=subs_.shadowCullManager->exchangeGeneration(*shadowCulling);assert(ok);}
  if(ray){const bool ok=subs_.rayShadowManager->exchangeGeneration(*ray);assert(ok);}
  if(lightBindings){const bool ok=lighting.exchangeRayShadowBindings(*lightBindings);assert(ok);}
  if(ddgi){const bool ok=subs_.ddgiManager->publishSceneHistoryReset(*ddgi);assert(ok);}
  {const bool ok=subs_.frameResourceManager->publishAuxiliaryBindings(auxiliaryBindings);assert(ok);}
  if(auxiliary) subs_.pbrAuxiliaryPass.swap(auxiliary);
  subs_.sceneProviderSynchronizer->exchangeState(providers,*subs_.sceneProviderRegistry,registry);
  subs_.frameRuntimeResourceRegistry->swapState(runtime);std::swap(meshProviderAssetCache_,assetCache);
  if(routing) {
    buffers_.objects.swap(nextObjects);buffers_.objectCapacities.swap(nextCapacities);buffers_.shadowObjectDescriptorReady.swap(shadowReady);
    for(auto& old:nextObjects) if(old.buffer && !controller.ownsObjectBuffer(old.buffer) &&
        std::ranges::none_of(routing->objectBuffers(),[&](const auto& owned){return owned.buffer==old.buffer;})) svc_.allocationManager.destroyBuffer(old);
  }
  ddgiSceneChanges_.clear();ddgiTrackedSceneRevision_=0;
  if(subs_.temporalManager) subs_.temporalManager->reset(std::move(reason));
  lastEditedSourceMaterial_=publication.editedSource;++materialPublications_;
  return true;
}

bool RendererFrontend::replayMaterialEdit(const MaterialEditHistory& state,GltfMaterialEditSide side) {
  if(!subs_.sceneManager || sceneGraph_.identity()!=state.graph || subs_.sceneManager->materialAssetIdentity()!=state.asset) return false;
  if(!container::material::TextureImageOwnershipValid(state.images,svc_.allocationManager)) return false;
  if(!state.references || !state.references->valid() || !state.references->active() ||
      !state.references->resourceDependenciesMatch(subs_.sceneManager->resourceLifecycleGeneration())) return false;
  const auto mapping=subs_.sceneManager->sourceMaterialRuntimeMapping();
  const auto source=state.command->destinationMaterial;
  if(source<0 || size_t(source)>=mapping.size() || subs_.sceneManager->isLinkedMaterialReadOnly(mapping[size_t(source)])) return false;
  return publishMaterialEdit(subs_.sceneManager->rebaseMaterialEdit(*state.command,side));
}

bool RendererFrontend::authorMaterialBinding(const container::material::GltfMaterialBindingRequest& request) {
  finishLightEditInput();
  using Operation=container::material::MaterialBindingEditOperation;
  using namespace container::scene;
  if(!subs_.sceneManager) throw std::runtime_error("Material assignment requires an initialized scene renderer");
  if(request.operation!=Operation::Assign && request.operation!=Operation::MakeUnique)
    throw std::invalid_argument("Unknown material binding operation");
  if(subs_.sceneManager->isLinkedNodeReadOnly(sceneGraph_,request.graphNode))
    throw std::invalid_argument("This linked asset is read-only. Use Make Editable first.");
  if(pendingSceneRetirement_ || sceneEditPublicationPending_) publishSceneEdit();
  ensureSceneEditHistory();
  auto& manager=*subs_.sceneManager;
  const auto* node=sceneGraph_.getNode(request.graphNode);
  if(!node || !node->renderable || sceneGraph_.isNodeRetired(request.graphNode) ||
      node->materialIndex!=request.expectedRuntimeMaterial || node->materialIndex>=manager.materialCount() || !manager.materialIsActive(node->materialIndex))
    throw std::invalid_argument("The material surface binding changed before assignment");
  const auto currentSource=manager.sourceMaterialForRuntime(node->materialIndex);
  const bool unique=request.operation==Operation::MakeUnique;
  if(request.runtimeMaterial>=manager.materialCount() || !manager.materialIsActive(request.runtimeMaterial) || request.sourceMaterial<-1 ||
      manager.sourceMaterialForRuntime(request.runtimeMaterial)!=request.sourceMaterial)
    throw std::invalid_argument("The requested source/runtime material pair is stale");
  if(!unique && request.name) throw std::invalid_argument("Assign cannot rename a material");
  if(unique && (request.runtimeMaterial!=node->materialIndex || request.sourceMaterial!=currentSource))
    throw std::invalid_argument("Make Unique must use the surface's current material pair");
  if(!unique && request.runtimeMaterial==node->materialIndex) return true;
  if(!unique) validateBindingFrameReuse(manager,*node,request.runtimeMaterial);
  if(!materialBindingBudget_) materialBindingBudget_=std::make_shared<MaterialBindingBudget>();
  auto budget=materialBindingBudget_;
  if(budget->reserved>=MaterialBindingBudget::limit) throw std::length_error("Material binding history budget is exhausted");
  const auto available=MaterialBindingBudget::limit-budget->reserved;
  MaterialPublication publication;
  auto state=std::make_shared<MaterialBindingHistory>();state->graph=sceneGraph_.identity();state->asset=manager.materialAssetIdentity();
  const std::array selectedMaterials{node->materialIndex,request.runtimeMaterial};
  state->images=manager.retainMaterialImages(selectedMaterials,available);
  if(state->images->ownedBytes()>=available) throw std::length_error("Material binding history has no logical resource budget");
  state->references=manager.retainMaterialResourceReferences(selectedMaterials,available-state->images->ownedBytes());
  uint32_t replacement=request.runtimeMaterial;
  std::optional<MaterialBindingAppendTarget> append;
  if(unique) {
    GltfMaterialPatch patch;
    if(request.name) patch.name=request.name;
    else if(currentSource>=0) {
      const auto& original=manager.animationSource().materials[size_t(currentSource)].name;
      size_t end=std::min<size_t>(original.size(),4000);
      while(end<original.size() && end && (uint8_t(original[end])&0xc0)==0x80) --end;
      patch.name=original.empty()?"Material copy":original.substr(0,end)+" copy";
    } else patch.name="Material copy";
    GltfMaterialAuthoringLimits authoring;authoring.maxOutputBytes=std::min(authoring.maxOutputBytes,available);
    MaterialGenerationLimits limits;limits.authoring=authoring;
    if(currentSource>=0) {
      const auto plan=manager.planMaterialMakeUnique(currentSource,patch,authoring);
      publication.material.emplace(manager.prepareMaterialGeneration(plan,gltfAnimation_.get(),limits));
    } else publication.material.emplace(manager.prepareRuntimeMaterialMakeUnique(request.runtimeMaterial,patch,gltfAnimation_.get(),limits));
    state->appended=publication.material->appendCommand();
    if(!state->appended) throw std::runtime_error("Make Unique did not prepare an owned material append");
    replacement=publication.material->runtimeMaterial();publication.editedSource=publication.material->sourceMaterial();
    append=MaterialBindingAppendTarget{uint32_t(publication.editedSource),replacement};
  } else publication.editedSource=request.sourceMaterial;
  const MaterialBindingRequest binding{request.graphNode,node->materialIndex,replacement};
  MaterialBindingLimits limits;limits.maxOutputBytes=std::min(limits.maxOutputBytes,available);
  state->bindings=PlanMaterialBindings(sceneGraph_,materialBindingContext(manager),std::span(&binding,1),append,limits);
  publication.bindings.emplace(PrepareMaterialBindings(sceneGraph_,materialBindingContext(manager),state->bindings,limits));
  const auto bindingBytes=state->bindings.ownedBytes(),appendBytes=state->appended.ownedBytes(),imageBytes=state->images->ownedBytes();
  const auto referenceBytes=state->references->ownedBytes();
  constexpr uint64_t overhead=sizeof(MaterialBindingHistory)+1024;
  if(bindingBytes>available || appendBytes>available-bindingBytes || imageBytes>available-bindingBytes-appendBytes ||
      referenceBytes>available-bindingBytes-appendBytes-imageBytes ||
      overhead>available-bindingBytes-appendBytes-imageBytes-referenceBytes)
    throw std::length_error("Material binding history budget is exhausted");
  state->reservation=bindingBytes+appendBytes+imageBytes+referenceBytes+overhead;state->budget=budget;budget->reserved+=state->reservation;
  container::util::EditHistory::Command command{unique?"Make material unique":"Assign material",
      [this,state]{return replayMaterialBinding(*state,false);},[this,state]{return replayMaterialBinding(*state,true);},true};
  if(!publishMaterialTransaction(publication)) return false;
  // All successor payloads, closures and logical reservation exist before the
  // previous gesture or redo branch can be released.
  sceneEdits_->finishGesture();finishMaterialGesture();
  sceneEdits_->recordAppliedPublished(std::move(command));
  return true;
}

bool RendererFrontend::replayMaterialBinding(const MaterialBindingHistory& state,bool redo) {
  using namespace container::scene;
  if(!subs_.sceneManager || state.graph!=sceneGraph_.identity() || state.asset!=subs_.sceneManager->materialAssetIdentity()) return false;
  if(!container::material::TextureImageOwnershipValid(state.images,svc_.allocationManager)) return false;
  if(!state.references || !state.references->valid() || !state.references->active() ||
      !state.references->resourceDependenciesMatch(subs_.sceneManager->resourceLifecycleGeneration())) return false;
  auto& manager=*subs_.sceneManager;
  if(std::ranges::any_of(state.bindings.assignments(),[&](const auto& assignment) {
      return manager.isLinkedNodeReadOnly(sceneGraph_,assignment.graphNode);})) return false;
  MaterialPublication publication;
  publication.bindings.emplace(RebaseMaterialBindings(sceneGraph_,materialBindingContext(manager),state.bindings,
      redo?MaterialBindingSide::After:MaterialBindingSide::Before));
  if(state.appended) publication.material.emplace(manager.prepareMaterialTailChange(state.appended,
      redo?MaterialTailState::Present:MaterialTailState::Detached,sceneGraph_,gltfAnimation_.get(),&*publication.bindings));
  const auto assignments=publication.bindings->assignments();
  if(!assignments.empty()) publication.editedSource=assignments.front().afterSourceMaterial;
  return publishMaterialTransaction(publication);
}
void RendererFrontend::finishMaterialGesture() {
  if(!materialGesture_) return;
  auto& pending=*materialGesture_;
  if(subs_.sceneManager && pending.state->graph==sceneGraph_.identity() && pending.state->asset==subs_.sceneManager->materialAssetIdentity()) {
    if(pending.state->command->changed) sceneEdits_->recordAppliedPublished(std::move(pending.history));
  }
  materialGesture_.reset();
}
bool RendererFrontend::authorMaterial(const container::material::GltfMaterialEditRequest& request) {
  finishLightEditInput();
  using Phase=container::material::MaterialEditGesturePhase;
  if(!subs_.sceneManager || request.sourceMaterial<0) throw std::invalid_argument("Choose an authored material before editing");
  if(!subs_.sceneManager->authoredMaterialIsActive(request.sourceMaterial)) throw std::invalid_argument("The selected material is retired");
  const auto mapping=subs_.sceneManager->sourceMaterialRuntimeMapping();
  if(size_t(request.sourceMaterial)<mapping.size() && subs_.sceneManager->isLinkedMaterialReadOnly(mapping[size_t(request.sourceMaterial)]))
    throw std::invalid_argument("This linked asset is read-only. Use Make Editable first.");
  if(!request.patch.textures.empty()) throw std::invalid_argument("Use a texture-slot request to edit resource bindings");
  if(request.phase!=Phase::Commit && request.phase!=Phase::Begin && request.phase!=Phase::Update && request.phase!=Phase::End && request.phase!=Phase::Cancel)
    throw std::invalid_argument("Unknown material gesture phase");
  const bool gesture=request.phase!=Phase::Commit;
  if(gesture && (!request.gestureId || !*request.gestureId || *request.gestureId>9007199254740991ull))
    throw std::invalid_argument("Material gesture needs an exact positive ID");
  if(request.phase==Phase::Commit && request.gestureId) throw std::invalid_argument("Standalone material edit cannot carry a gesture ID");
  if((request.phase==Phase::Commit || request.phase==Phase::Begin || request.phase==Phase::Update) && !hasPatch(request.patch))
    throw std::invalid_argument("Material edit contains no property changes");
  if(request.phase==Phase::Cancel && hasPatch(request.patch)) throw std::invalid_argument("Material cancellation cannot contain a patch");
  if(pendingSceneRetirement_ || sceneEditPublicationPending_) publishSceneEdit();
  ensureSceneEditHistory();
  const bool startsGesture=request.phase==Phase::Commit || request.phase==Phase::Begin;
  if(gesture && request.phase!=Phase::Begin && (!materialGesture_ || materialGesture_->id!=*request.gestureId || materialGesture_->source!=request.sourceMaterial))
    throw std::invalid_argument("Material gesture no longer matches the active edit");
  if(request.phase==Phase::Cancel) {
    auto pending=materialGesture_;
    if(pending->state->command->changed && !replayMaterialEdit(*pending->state,GltfMaterialEditSide::Before)) return false;
    materialGesture_.reset();return true;
  }
  if(request.phase==Phase::End && !hasPatch(request.patch)) {finishMaterialGesture();return true;}
  if(!materialBindingBudget_) materialBindingBudget_=std::make_shared<MaterialBindingBudget>();
  if(materialBindingBudget_->reserved>=MaterialBindingBudget::limit) throw std::length_error("Material history budget is exhausted");
  const auto available=MaterialBindingBudget::limit-materialBindingBudget_->reserved;
  GltfMaterialAuthoringLimits authoring;authoring.maxOutputBytes=std::min(authoring.maxOutputBytes,available/3);
  if(!authoring.maxOutputBytes) throw std::length_error("Material gesture has no remaining plan storage budget");
  const auto plan=subs_.sceneManager->planMaterialEdit(request.sourceMaterial,request.patch,authoring);
  auto command=std::make_shared<const GltfMaterialAuthoringPlan>(materialGesture_ && !startsGesture?
      subs_.sceneManager->coalesceMaterialEdits(*materialGesture_->state->command,plan,authoring):plan);
  // Every successor owns its complete selected endpoint union and callbacks
  // before publication. A failed update cannot change the preceding preview,
  // undo endpoint, image pins, reservation or redo cursor.
  auto pending=std::make_shared<PendingMaterialGesture>();pending->id=request.gestureId.value_or(0);pending->source=request.sourceMaterial;
  if(materialGesture_ && !startsGesture) pending->uiTarget=materialGesture_->uiTarget;
  pending->state=std::make_shared<MaterialEditHistory>();pending->state->graph=sceneGraph_.identity();
  pending->state->asset=subs_.sceneManager->materialAssetIdentity();pending->state->command=command;
  pending->state->images=subs_.sceneManager->retainMaterialPlanImages(*command,nullptr,available);
  const auto beforeReferences=uint64_t(sizeof(PendingMaterialGesture)+sizeof(MaterialEditHistory)+1024)+command->peakOwnedBytes+pending->state->images->ownedBytes();
  if(beforeReferences>=available) throw std::length_error("Material gesture has no logical resource history budget");
  pending->state->references=subs_.sceneManager->retainMaterialPlanResourceReferences(*command,nullptr,available-beforeReferences);
  const auto bytes=beforeReferences+pending->state->references->ownedBytes();
  if(bytes>available) throw std::length_error("Material gesture history exceeds its retained resource budget");
  pending->state->budget=materialBindingBudget_;pending->state->reservation=bytes;materialBindingBudget_->reserved+=bytes;
  auto state=pending->state;
  pending->history={"Edit material",[this,state]{return replayMaterialEdit(*state,GltfMaterialEditSide::Before);},
      [this,state]{return replayMaterialEdit(*state,GltfMaterialEditSide::After);},true};
  if(!publishMaterialEdit(plan)) return false;
  // A failed new command must retain the preceding preview and history cursor.
  // Its flat command and all replacement captures are ready before publication.
  if(startsGesture) {sceneEdits_->finishGesture();finishMaterialGesture();}
  pending->authoredRevision=subs_.sceneManager->authoredMaterialRevision();
  materialGesture_.swap(pending);
  if(request.phase==Phase::Commit || request.phase==Phase::End) finishMaterialGesture();
  return true;
}

void RendererFrontend::refreshResourceCatalog() {
  if(!subs_.guiManager) return;
  if(resourceCatalogPreview_ && resourceCatalogPreview_->matches(*this)) return;
  // A failed key/proof drops the previous borrowed/provider witnesses before
  // any replacement preparation. This cache owns no image or logical pins.
  resourceCatalogPreview_.reset();
  using D=container::ui::ResourceCatalogDomain;using K=container::ui::ResourceCatalogKind;
  using Domain=container::util::ResourceDomainKind;using Kind=container::util::ResourceKind;
  container::ui::ResourceCatalogSnapshot snapshot;
  std::shared_ptr<MaterialExternalProof> externalProof;
  try {
    if(!subs_.sceneManager) throw std::invalid_argument("Load a scene before managing its resources");
    const auto& manager=*subs_.sceneManager;const auto generation=manager.resourceLifecycleGeneration();
    if(!generation || !generation->valid()) throw std::invalid_argument("The resource catalog is not ready");
    snapshot.assetIdentity=manager.materialAssetIdentity();snapshot.resourceRevision=manager.materialResourceRevision();
    if(resourceCatalogSelection_ && resourceCatalogSelection_->assetIdentity!=snapshot.assetIdentity) {
      resourceCatalogSelection_.reset();resourceCatalogError_.clear();
    }
    const auto archive=manager.authoredMaterialResources();const auto& evaluated=manager.animationSource();
    size_t slots=0;uint64_t bytes=0;
    const auto add=[&](Domain domain,Kind kind,D uiDomain,K uiKind,size_t count,const auto& label) {
      if(count!=generation->rowCount(domain,kind) || count>1'000'000 || count>1'000'000-slots)
        throw std::length_error("Resource catalog row identities exceed the editor limit");
      slots+=count;
      for(uint32_t row=0;row<count;++row) {
        const container::scene::TextureResourceSelection selected{domain,kind,row};if(!generation->active(selected)) continue;
        if(snapshot.choices.size()==65536) throw std::length_error("The active resource catalog exceeds the editor row limit");
        const auto handle=generation->resource(selected);if(!handle) throw std::invalid_argument("An active resource has no logical identity");
        auto text=label(row);bytes+=sizeof(container::ui::ResourceCatalogChoice)+text.size()+128;
        if(text.size()>256 || bytes>16ull*1024*1024) throw std::length_error("Resource catalog labels exceed the editor storage limit");
        container::ui::ResourceCatalogTarget target{snapshot.assetIdentity,snapshot.resourceRevision,
            handle.domain().identity(),handle.incarnation(),row,uiDomain,uiKind};
        snapshot.choices.push_back({target,std::move(text)});
      }
    };
    add(Domain::Archive,Kind::Texture,D::Archive,K::Texture,archive.textures.size(),[&](uint32_t row) {
      return resourceLabel(materialTextureFile(archive,row),"Imported texture",row);
    });
    add(Domain::Archive,Kind::Image,D::Archive,K::Image,archive.images.size(),[&](uint32_t row) {
      const auto& image=archive.images[row];const auto text=!image.name.empty()?std::string_view(image.name):
          (!std::string_view(image.uri).starts_with("data:")?std::string_view(image.uri):std::string_view{});
      return resourceLabel(text,"Imported image",row);
    });
    add(Domain::Archive,Kind::Sampler,D::Archive,K::Sampler,archive.samplers.size(),[&](uint32_t row) {
      return resourceLabel(archive.samplers[row].name,"Imported sampler",row);
    });
    add(Domain::Evaluated,Kind::Texture,D::Evaluated,K::Texture,evaluated.textures.size(),[&](uint32_t row) {
      return resourceLabel(evaluated.textures[row].name,"Evaluated texture",row);
    });
    add(Domain::Evaluated,Kind::Image,D::Evaluated,K::Image,evaluated.images.size(),[&](uint32_t row) {
      return resourceLabel(evaluated.images[row].name,"Evaluated image",row);
    });
    add(Domain::Evaluated,Kind::Sampler,D::Evaluated,K::Sampler,evaluated.samplers.size(),[&](uint32_t row) {
      return resourceLabel(evaluated.samplers[row].name,"Evaluated sampler",row);
    });
    add(Domain::Runtime,Kind::RuntimeTexture,D::Runtime,K::RuntimeTexture,manager.materialTextureResourceCount(),[&](uint32_t row) {
      return resourceLabel({},"Sampled texture",row);
    });
    auto selected=snapshot.choices.end();
    if(resourceCatalogSelection_) selected=std::ranges::find_if(snapshot.choices,[&](const auto& row) {
      return sameResourceSelection(row.target,*resourceCatalogSelection_);
    });
    if(selected==snapshot.choices.end() && !snapshot.choices.empty()) selected=snapshot.choices.begin();
    snapshot.available=true;snapshot.error=boundedResourceText(resourceCatalogError_);
    if(selected!=snapshot.choices.end()) {
      snapshot.selected=selected->target;resourceCatalogSelection_=selected->target;
      try {
        if(!subs_.sceneProviderRegistry) throw std::invalid_argument("The resource provider registry is unavailable");
        externalProof=std::make_shared<MaterialExternalProof>(subs_.bimManager.get(),*subs_.sceneProviderRegistry,
            manager.materialCount(),false,std::nullopt,&manager);
        const auto planned=manager.planResourceRetirement(resourceSelection(*snapshot.selected),gltfAnimation_.get(),64ull*1024*1024);
        (void)resolveResourceTarget(manager,*snapshot.selected);
        if(planned.empty() || !externalProof->matches(*subs_.sceneProviderRegistry)) throw std::invalid_argument("Resource consumers changed during the unused-resource check");
        snapshot.retirementAvailable=true;
      } catch(const std::exception& error) {snapshot.retirementReason=boundedResourceText(error.what());}
    } else {resourceCatalogSelection_.reset();snapshot.retirementReason="No active resources are available";}
  } catch(const std::exception& error) {
    snapshot.available=false;snapshot.retirementAvailable=false;snapshot.selected.reset();snapshot.choices.clear();
    snapshot.error=boundedResourceText(error.what());resourceCatalogSelection_.reset();
  }
  std::shared_ptr<ResourceCatalogPreview> preview;
  try {preview=ResourceCatalogPreview::capture(*this,snapshot,std::move(externalProof));}
  catch(const std::exception&) { /* An advisory cache never changes availability or authoring authority. */ }
  subs_.guiManager->setResourceCatalogSnapshot(std::move(snapshot));
  resourceCatalogPreview_=std::move(preview);
}

void RendererFrontend::refreshMaterialInspector() {
  if(!subs_.guiManager) return;
  refreshResourceCatalog();
  container::ui::MaterialInspectorSnapshot snapshot;
  snapshot.target.selectedGraphNode=sceneState_.selectedMeshNode;
  snapshot.target.graphIdentity=sceneGraph_.identity();
  if(subs_.sceneManager) {
    snapshot.target.assetIdentity=subs_.sceneManager->materialAssetIdentity();
    snapshot.target.authoredRevision=subs_.sceneManager->authoredMaterialRevision();
  }
  try {
    const auto selected=sceneState_.selectedMeshNode;
    const auto* selectedNode=sceneGraph_.getNode(selected);
    if(!subs_.sceneManager) {
      materialInspectorBinding_=UINT32_MAX;
      subs_.guiManager->setMaterialInspectorSnapshot(std::move(snapshot));return;
    }
    const auto& manager=*subs_.sceneManager;
    const auto& source=manager.animationSource();
    const auto mapping=manager.sourceMaterialRuntimeMapping();
    if(mapping.size()!=source.materials.size() || mapping.size()>size_t(INT32_MAX) ||
        manager.materialCount()>16ull*1024*1024/sizeof(int32_t))
      throw std::runtime_error("Material inspector identity table exceeds its limits");
    // One compact inverse avoids a full source-table search per visible surface.
    std::vector<int32_t> sourceIds(manager.materialCount(),-1);
    for(size_t i=0;i<mapping.size();++i) {
      const auto runtime=mapping[i];
      if(runtime>=sourceIds.size() || sourceIds[runtime]!=-1)
        throw std::runtime_error("Material inspector source bindings are ambiguous");
      sourceIds[runtime]=int32_t(i);
    }
    if(selectedNode && !sceneGraph_.isNodeRetired(selected) && sceneGraph_.isNodeInActiveHierarchy(selected)) {
    std::vector<uint32_t> pending{selected};uint64_t visits=0,edges=0,choiceBytes=0;
    constexpr size_t maxChoices=65536;
    while(!pending.empty()) {
      const auto id=pending.back();pending.pop_back();
      if(++visits>262144) throw std::runtime_error("Material inspector subtree exceeds its node limit");
      const auto* node=sceneGraph_.getNode(id);
      if(!node || sceneGraph_.isNodeRetired(id) || !sceneGraph_.isNodeInActiveHierarchy(id)) continue;
      if(node->renderable && sceneGraph_.isNodeActive(id)) {
        if(snapshot.choices.size()==maxChoices || node->materialIndex>=sourceIds.size() || !manager.materialIsActive(node->materialIndex))
          throw std::runtime_error("Material inspector surface list exceeds its limits");
        size_t labelEnd=std::min<size_t>(node->name.size(),192);
        while(labelEnd<node->name.size() && labelEnd && (uint8_t(node->name[labelEnd])&0xc0)==0x80) --labelEnd;
        auto label=node->name.substr(0,labelEnd);
        if(label.empty()) label="Surface";
        label+=" · Node "+std::to_string(id)+" · Material "+std::to_string(node->materialIndex);
        choiceBytes+=sizeof(container::ui::MaterialBindingChoice)+label.size();
        if(choiceBytes>16ull*1024*1024) throw std::runtime_error("Material inspector surface labels exceed their storage limit");
        snapshot.choices.push_back({id,node->materialIndex,sourceIds[node->materialIndex],std::move(label)});
      }
      edges+=node->children.size();
      if(edges>524288 || pending.size()+node->children.size()>262144)
        throw std::runtime_error("Material inspector subtree exceeds its edge limit");
      pending.insert(pending.end(),node->children.rbegin(),node->children.rend());
    }
    auto binding=std::ranges::find(snapshot.choices,materialInspectorBinding_,&container::ui::MaterialBindingChoice::graphNode);
    if(binding==snapshot.choices.end()) {
      binding=std::ranges::find_if(snapshot.choices,[](const auto& choice){return choice.sourceMaterial>=0;});
      if(binding==snapshot.choices.end() && !snapshot.choices.empty()) binding=snapshot.choices.begin();
    }
    if(binding!=snapshot.choices.end()) {
      materialInspectorBinding_=binding->graphNode;
      snapshot.target.bindingGraphNode=binding->graphNode;snapshot.target.runtimeMaterial=binding->runtimeMaterial;
      snapshot.target.sourceMaterial=binding->sourceMaterial;
      if(binding->sourceMaterial>=0) {
        uint32_t sharedUsers=0;
        for(const auto id:sceneGraph_.renderableNodes()) {
          const auto* node=sceneGraph_.getNode(id);
          if(node && !sceneGraph_.isNodeRetired(id) && node->materialIndex==binding->runtimeMaterial) ++sharedUsers;
        }
        std::array<bool,size_t(GltfMaterialProperty::Count)> animated{};
        std::array<bool,size_t(GltfMaterialExtension::Count)> animatedExtensions{};
        if(gltfAnimation_) {
          const auto& clips=gltfAnimation_->library().clips;const auto active=gltfAnimation_->selectedClip();
          for(size_t c=0;c<clips.size();++c) for(const auto& track:clips[c].tracks) {
            if(track.target.kind!=AnimationTargetKind::Material || track.target.index!=size_t(binding->sourceMaterial)) continue;
            for(const auto& info:GltfMaterialPropertyRegistry()) if(track.target.property==info.path && active==c) animated[size_t(info.property)]=true;
          }
        }
        // Unsupported future pointer leaves may be absent from the decoded
        // player, but still make removing their owning extension unsafe.
        uint64_t channels=0;
        for(const auto& animation:source.animations) for(const auto& channel:animation.channels) {
          if(++channels>1'000'000) throw std::runtime_error("Material inspector animation traversal exceeds its limit");
          const auto pointer=channel.target_extensions.find("KHR_animation_pointer");
          if(pointer==channel.target_extensions.end() || !pointer->second.IsObject()) continue;
          const auto& value=pointer->second.Get("pointer");if(!value.IsString()) continue;
          const std::string_view path=value.Get<std::string>();constexpr std::string_view prefix="/materials/";
          if(!path.starts_with(prefix)) continue;
          const auto slash=path.find('/',prefix.size());if(slash==std::string_view::npos) continue;
          uint32_t materialId{};const auto number=path.substr(prefix.size(),slash-prefix.size());
          const auto parsed=std::from_chars(number.data(),number.data()+number.size(),materialId);
          if(parsed.ec!=std::errc{} || parsed.ptr!=number.data()+number.size() || materialId!=uint32_t(binding->sourceMaterial)) continue;
          const auto property=path.substr(slash+1);if(!property.starts_with("extensions/")) continue;
          const auto relative=property.substr(11);
          for(size_t e=0;e<animatedExtensions.size();++e) {
            const auto name=GltfMaterialExtensionName(GltfMaterialExtension(e));
            if(relative.starts_with(name) && relative.size()>name.size() && relative[name.size()]=='/') animatedExtensions[e]=true;
          }
        }
        snapshot=container::ui::BuildMaterialInspectorSnapshot(snapshot.target,manager.authoredMaterial(binding->sourceMaterial),
            source.materials[size_t(binding->sourceMaterial)],sharedUsers,snapshot.choices,animated,animatedExtensions);
        snapshot.resourceRevision=manager.materialResourceRevision();snapshot.textureEditingAvailable=true;
        const auto catalog=manager.authoredMaterialResources();
        for(const auto& info:GltfMaterialTextureSlotRegistry()) {
          auto& slot=snapshot.textureSlots[size_t(info.slot)];slot.available=true;
          slot.mappingEditable=slot.bound;slot.samplerEditable=slot.bound;
          if(slot.authoredTexture>=0) {
            slot.file=materialTextureFile(catalog,uint32_t(slot.authoredTexture));
            slot.sampler=materialTextureSampler(catalog,uint32_t(slot.authoredTexture));
          }
          if(manager.materialRenderProperties(binding->runtimeMaterial).specularGlossiness &&
              info.slot==GltfMaterialTextureSlot::MetallicRoughness) {
            slot.available=false;slot.error="Specular-glossiness uses its shared specular/glossiness texture instead.";
          }
        }
        try {
          if(catalog.textures.size()>65536) throw std::length_error("Texture catalog exceeds the inspector limit");
          uint64_t bytes=catalog.textures.size()*sizeof(container::ui::MaterialTextureChoice);
          snapshot.textureChoices.reserve(catalog.textures.size());
          for(uint32_t texture=0;texture<catalog.textures.size();++texture) {
            if(!catalog.textureActive.empty() &&
                (catalog.textureActive.size()!=catalog.textures.size() || catalog.textureActive[texture]!=1)) continue;
            const auto file=materialTextureFile(catalog,texture);
            auto label=file.substr(0,std::min<size_t>(file.size(),160));
            while(!label.empty() && label.size()<file.size() && (uint8_t(file[label.size()])&0xc0)==0x80) label.pop_back();
            label+=" · "+std::to_string(texture);bytes+=label.size()+file.size();
            if(bytes>16ull*1024*1024) throw std::length_error("Texture catalog exceeds the inspector byte limit");
            const auto image=materialTextureImage(catalog.textures[texture]);
            const bool retained=image>=0 && size_t(image)<catalog.images.size() &&
                (!catalog.images[size_t(image)].image.empty() || catalog.images[size_t(image)].bufferView>=0 ||
                    std::string_view(catalog.images[size_t(image)].uri).starts_with("data:"));
            const bool color=retained || (texture<catalog.colorResources.size() && catalog.colorResources[texture]!=UINT32_MAX);
            const bool data=retained || (texture<catalog.dataResources.size() && catalog.dataResources[texture]!=UINT32_MAX);
            snapshot.textureChoices.push_back({texture,texture<catalog.archiveToEvaluated.size()?catalog.archiveToEvaluated[texture]:-1,
                std::move(label),file,color,data});
          }
        } catch(const std::length_error&) {
          snapshot.textureChoices.clear();
          snapshot.textureEditingError="The existing texture list is too large to display. File import, replacement and slot settings remain available.";
        }
        if(materialTextureErrorAsset_==manager.materialAssetIdentity() && materialTextureErrorSource_==binding->sourceMaterial &&
            !materialTextureError_.empty()) {
          if(!snapshot.textureEditingError.empty()) snapshot.textureEditingError+='\n';
          snapshot.textureEditingError+=materialTextureError_;
          if(snapshot.textureEditingError.size()>4096) snapshot.textureEditingError.resize(4096);
        }
      } else snapshot.error="Use Make unique to create an editable material for this surface, or assign an existing material.";
      if(manager.materialCount()<=65536) {
        snapshot.assignmentChoices.reserve(manager.materialCount());
        uint64_t labels{};
        for(uint32_t runtime=0;runtime<manager.materialCount();++runtime) {
          if(!manager.materialIsActive(runtime)) continue;
          const auto sourceId=sourceIds[runtime];
          std::string label;
          if(sourceId>=0) {
            const auto& name=source.materials[size_t(sourceId)].name;
            size_t end=std::min<size_t>(name.size(),192);
            while(end<name.size() && end && (uint8_t(name[end])&0xc0)==0x80) --end;
            label=name.substr(0,end);
          }
          if(label.empty()) label=sourceId<0?"Runtime material":"Material";
          label+=" · "+std::to_string(runtime);
          labels+=sizeof(container::ui::MaterialAssignmentChoice)+label.size();
          if(labels>16ull*1024*1024) throw std::length_error("Material assignment catalog exceeds its storage limit");
          snapshot.assignmentChoices.push_back({sourceId,runtime,std::move(label)});
        }
        snapshot.bindingOperationsAvailable=true;
        snapshot.makeUniqueAvailable=(snapshot.target.sourceMaterial>=0 && snapshot.available) ||
            manager.hasRuntimeMaterialProvenance(snapshot.target.runtimeMaterial);
      } else snapshot.bindingOperationsError="The material list exceeds the current editor limit.";
    } else materialInspectorBinding_=UINT32_MAX;
    } else materialInspectorBinding_=UINT32_MAX;
    if(materialCatalogAsset_!=manager.materialAssetIdentity()) {
      materialCatalogAsset_=manager.materialAssetIdentity();materialCatalogRuntime_=UINT32_MAX;materialCatalogError_.clear();
    }
    if(manager.materialCount()>65536) snapshot.materialCatalogError="The material catalog exceeds the current editor limit.";
    else {
      uint64_t bytes=0;snapshot.catalogChoices.reserve(manager.materialCount());
      for(uint32_t runtime=0;runtime<manager.materialCount();++runtime) {
        if(!manager.materialIsActive(runtime)) continue;
        const auto sourceId=sourceIds[runtime];std::string label;
        if(sourceId>=0) {
          const auto& name=source.materials[size_t(sourceId)].name;size_t end=std::min<size_t>(name.size(),192);
          while(end<name.size() && end && (uint8_t(name[end])&0xc0)==0x80) --end;label=name.substr(0,end);
        }
        if(label.empty()) label=sourceId<0?"Runtime material":"Material";
        label+=" · "+std::to_string(runtime);bytes+=sizeof(container::ui::MaterialAssignmentChoice)+label.size();
        if(bytes>8ull*1024*1024) throw std::length_error("Material catalog labels exceed the editor storage limit");
        snapshot.catalogChoices.push_back({sourceId,runtime,std::move(label)});
      }
      auto choice=std::ranges::find(snapshot.catalogChoices,materialCatalogRuntime_,&container::ui::MaterialAssignmentChoice::runtimeMaterial);
      if(choice==snapshot.catalogChoices.end()) {
        choice=std::ranges::find_if(snapshot.catalogChoices,[](const auto& row){return row.sourceMaterial>=0;});
        if(choice==snapshot.catalogChoices.end() && !snapshot.catalogChoices.empty()) choice=snapshot.catalogChoices.begin();
        materialCatalogError_.clear();
      }
      if(choice!=snapshot.catalogChoices.end()) {
        materialCatalogRuntime_=choice->runtimeMaterial;
        snapshot.catalogTarget={sceneGraph_.identity(),manager.materialAssetIdentity(),manager.authoredMaterialRevision(),manager.materialRevision(),
            choice->runtimeMaterial,choice->sourceMaterial};
        snapshot.materialRetirementReason=manager.materialRetirementBlockReason(choice->runtimeMaterial,sceneGraph_);
        if(snapshot.materialRetirementReason.empty()) snapshot.materialRetirementReason=
            externalMaterialRetirementReason(subs_.bimManager.get(),subs_.sceneProviderRegistry.get(),choice->runtimeMaterial);
        snapshot.materialRetirementAvailable=snapshot.materialRetirementReason.empty();snapshot.materialCatalogError=materialCatalogError_;
      } else materialCatalogRuntime_=UINT32_MAX;
    }
  } catch(const std::exception& error) {
    snapshot.available=false;snapshot.error=std::string(error.what()).substr(0,4096);
    snapshot.catalogChoices.clear();snapshot.catalogTarget={};snapshot.materialRetirementAvailable=false;
    snapshot.materialRetirementReason.clear();snapshot.materialCatalogError=snapshot.error;
  }
  subs_.guiManager->setMaterialInspectorSnapshot(std::move(snapshot));
}

void RendererFrontend::processMaterialEditRequests() {
  if(!subs_.guiManager) return;
  using Phase=container::material::MaterialEditGesturePhase;
  const auto requests=subs_.guiManager->consumeMaterialEditRequests();
  const auto selection=subs_.guiManager->consumeMaterialBindingSelectionRequest();
  const auto operation=subs_.guiManager->consumeMaterialBindingOperationRequest();
  const auto texture=subs_.guiManager->consumeMaterialTextureEditRequest();
  const auto catalogSelection=subs_.guiManager->consumeMaterialCatalogSelectionRequest();
  const auto retirement=subs_.guiManager->consumeMaterialRetirementRequest();
  const auto resourceSelectionRequest=subs_.guiManager->consumeResourceCatalogSelectionRequest();
  const auto resourceRetirement=subs_.guiManager->consumeResourceRetirementRequest();
  const auto batchRevision=subs_.sceneManager?subs_.sceneManager->authoredMaterialRevision():0;
  const auto batchMaterialRevision=subs_.sceneManager?subs_.sceneManager->materialRevision():0;
  bool batchSucceeded=true;
  const auto currentBinding=[&](const container::ui::MaterialInspectorTarget& target,bool requireSelection) {
    if(!subs_.sceneManager || target.graphIdentity!=sceneGraph_.identity() ||
        target.assetIdentity!=subs_.sceneManager->materialAssetIdentity()) return false;
    const auto* node=sceneGraph_.getNode(target.bindingGraphNode);
    if(!node || !node->renderable || !sceneGraph_.isNodeActive(target.bindingGraphNode) ||
        node->materialIndex!=target.runtimeMaterial || !subs_.sceneManager->materialIsActive(node->materialIndex) ||
        subs_.sceneManager->sourceMaterialForRuntime(node->materialIndex)!=target.sourceMaterial)
      return false;
    return !requireSelection || (target.selectedGraphNode==sceneState_.selectedMeshNode &&
        target.bindingGraphNode==materialInspectorBinding_ && bindingWithinSelection(sceneGraph_,target.selectedGraphNode,target.bindingGraphNode));
  };
  for(const auto& queued:requests) {
    try {
      auto request=queued.request;const auto& target=queued.target;
      if(resourceSelectionRequest || resourceRetirement) {
        if(request.phase==Phase::End && !hasPatch(request.patch)) continue;
        throw std::invalid_argument("A queued resource action cannot be combined with a material gesture");
      }
      if((texture || retirement || resourceRetirement) && request.phase==Phase::End) {
        // Deactivation before a texture click must not commit away the prior
        // gesture if decoding or later renderer preparation rejects that click.
        if(!hasPatch(request.patch)) continue;
        request.phase=Phase::Update;
      }
      // A cancellation generated while replacing the displayed selection may
      // still own the previous exact gesture. A completed gesture needs no work.
      if(request.phase==Phase::Cancel && !materialGesture_) continue;
      const bool ownGesture=subs_.sceneManager && materialGesture_ && materialGesture_->uiTarget && request.gestureId==materialGesture_->id &&
          request.sourceMaterial==materialGesture_->source && sameInspectorBinding(target,*materialGesture_->uiTarget) &&
          materialGesture_->authoredRevision==subs_.sceneManager->authoredMaterialRevision();
      if(request.sourceMaterial!=target.sourceMaterial || !currentBinding(target,request.phase!=Phase::Cancel) ||
          (target.authoredRevision!=subs_.sceneManager->authoredMaterialRevision() && target.authoredRevision!=batchRevision && !ownGesture) ||
          (request.phase==Phase::Cancel && !ownGesture))
        throw std::runtime_error("The selected material changed; refresh the inspector before editing");
      if(!authorMaterial(request)) throw std::runtime_error("The material edit could not be applied");
      if(request.phase==Phase::Begin && materialGesture_) materialGesture_->uiTarget=target;
      subs_.guiManager->setStatusMessage(request.phase==Phase::Cancel?"Cancelled material edit":"Updated material");
    } catch(const std::exception& error) {
      batchSucceeded=false;
      subs_.guiManager->setStatusMessage("Cannot edit material: "+std::string(error.what()));
    }
  }
  if(selection) {
    try {
      if(!currentBinding(selection->target,true) || selection->target.authoredRevision!=batchRevision ||
          !bindingWithinSelection(sceneGraph_,selection->target.selectedGraphNode,selection->bindingGraphNode))
        throw std::runtime_error("The material surface selection has changed");
      const auto* node=sceneGraph_.getNode(selection->bindingGraphNode);
      if(!node || !node->renderable || !sceneGraph_.isNodeActive(selection->bindingGraphNode))
        throw std::runtime_error("The selected material surface is no longer active");
      finishMaterialGesture();materialInspectorBinding_=selection->bindingGraphNode;
    } catch(const std::exception& error) {
      subs_.guiManager->setStatusMessage("Cannot select material: "+std::string(error.what()));
    }
  }
  if(operation) {
    try {
      if(!batchSucceeded || !currentBinding(operation->target,true) || operation->target.authoredRevision!=batchRevision)
        throw std::runtime_error("The selected material changed; refresh before assigning");
      const bool unique=operation->operation==container::ui::MaterialBindingOperation::MakeUnique;
      if(operation->operation!=container::ui::MaterialBindingOperation::Assign && !unique)
        throw std::invalid_argument("Unknown material binding operation");
      container::material::GltfMaterialBindingRequest request;
      request.graphNode=operation->target.bindingGraphNode;request.expectedRuntimeMaterial=operation->target.runtimeMaterial;
      request.operation=unique?container::material::MaterialBindingEditOperation::MakeUnique:container::material::MaterialBindingEditOperation::Assign;
      request.sourceMaterial=operation->sourceMaterial;request.runtimeMaterial=operation->runtimeMaterial;request.name=operation->name;
      if(!authorMaterialBinding(request)) throw std::runtime_error("The material binding could not be applied");
      subs_.guiManager->setStatusMessage(unique?"Made selected surface material unique":"Assigned material to selected surface");
    } catch(const std::exception& error) {
      subs_.guiManager->setStatusMessage("Cannot assign material: "+std::string(error.what()));
    }
  }
  if(texture) {
    try {
      if(!batchSucceeded || selection || operation || !currentBinding(texture->target,true) ||
          texture->target.authoredRevision!=batchRevision || texture->request.sourceMaterial!=texture->target.sourceMaterial)
        throw std::runtime_error("The selected material changed; refresh before editing its texture");
      if(!authorMaterialTexture(texture->request)) throw std::runtime_error("The texture edit could not be applied");
      materialTextureError_.clear();materialTextureErrorSource_=-1;materialTextureErrorAsset_=0;
      subs_.guiManager->setStatusMessage("Updated material texture");
    } catch(const std::exception& error) {
      materialTextureError_="Cannot edit texture: "+std::string(error.what());
      size_t end=std::min<size_t>(materialTextureError_.size(),4096);
      while(end<materialTextureError_.size() && end && (uint8_t(materialTextureError_[end])&0xc0)==0x80) --end;
      materialTextureError_.resize(end);materialTextureErrorSource_=texture->target.sourceMaterial;
      materialTextureErrorAsset_=texture->target.assetIdentity;
      subs_.guiManager->setStatusMessage(materialTextureError_);
    }
  }
  const auto currentCatalog=[&](const container::ui::MaterialCatalogTarget& target) {
    return subs_.sceneManager && target.graphIdentity==sceneGraph_.identity() && target.assetIdentity==subs_.sceneManager->materialAssetIdentity() &&
        target.authoredRevision==batchRevision && target.materialRevision==batchMaterialRevision &&
        target.runtimeMaterial==materialCatalogRuntime_ && subs_.sceneManager->materialIsActive(target.runtimeMaterial) &&
        subs_.sceneManager->sourceMaterialForRuntime(target.runtimeMaterial)==target.sourceMaterial;
  };
  if(catalogSelection) {
    try {
      if(!batchSucceeded || selection || operation || texture || retirement || !currentCatalog(catalogSelection->target) ||
          !subs_.sceneManager->materialIsActive(catalogSelection->runtimeMaterial) ||
          subs_.sceneManager->sourceMaterialForRuntime(catalogSelection->runtimeMaterial)!=catalogSelection->sourceMaterial)
        throw std::invalid_argument("The material catalog changed; refresh before choosing a material");
      materialCatalogRuntime_=catalogSelection->runtimeMaterial;materialCatalogError_.clear();
    } catch(const std::exception& error) {
      materialCatalogError_=std::string(error.what()).substr(0,4096);subs_.guiManager->setStatusMessage(materialCatalogError_);
    }
  }
  if(retirement) {
    try {
      if(!batchSucceeded || selection || operation || texture || catalogSelection || !currentCatalog(retirement->target))
        throw std::invalid_argument("The material catalog changed; refresh before deleting a material");
      if(!authorMaterialRetirement(retirement->target.runtimeMaterial,retirement->target.sourceMaterial,subs_.sceneManager->authoredMaterialRevision()))
        throw std::runtime_error("The material deletion could not be published");
      materialCatalogError_.clear();subs_.guiManager->setStatusMessage("Deleted unused material. Undo restores its definition.");
    } catch(const std::exception& error) {
      materialCatalogError_="Cannot delete material: "+std::string(error.what());materialCatalogError_.resize(std::min<size_t>(4096,materialCatalogError_.size()));
      subs_.guiManager->setStatusMessage(materialCatalogError_);
    }
  }
  const bool resourceConflict=std::ranges::any_of(requests,[](const auto& queued) {
    return hasPatch(queued.request.patch) || (queued.request.phase!=Phase::End && queued.request.phase!=Phase::Commit);
  });
  if(resourceSelectionRequest) {
    try {
      if(!batchSucceeded || resourceConflict || selection || operation || texture || catalogSelection || retirement || resourceRetirement ||
          !subs_.sceneManager) throw std::invalid_argument("Finish the current edit before choosing a resource");
      (void)resolveResourceTarget(*subs_.sceneManager,resourceSelectionRequest->target);
      resourceCatalogSelection_=resourceSelectionRequest->target;resourceCatalogError_.clear();
    } catch(const std::exception& error) {
      resourceCatalogError_=boundedResourceText(error.what());subs_.guiManager->setStatusMessage(resourceCatalogError_);
    }
  }
  if(resourceRetirement) {
    try {
      if(!batchSucceeded || resourceConflict || selection || operation || texture || catalogSelection || retirement || resourceSelectionRequest ||
          !resourceCatalogSelection_ || *resourceCatalogSelection_!=resourceRetirement->target)
        throw std::invalid_argument("The resource selection changed; refresh before deleting");
      if(!authorResourceRetirement(*resourceRetirement)) throw std::runtime_error("Resource deletion could not be published");
      resourceCatalogError_.clear();subs_.guiManager->setStatusMessage("Deleted unused resource. Undo retains its images until history is released.");
    } catch(const std::exception& error) {
      resourceCatalogError_=boundedResourceText("Cannot delete resource: "+std::string(error.what()));
      subs_.guiManager->setStatusMessage(resourceCatalogError_);
    }
  }
}

void RendererFrontend::appendMaterialTextureTelemetry(nlohmann::json& json) const {
  auto& result=json["gltfMaterialTextures"];
  result={{"resourceRevision",0},{"archiveImageCount",0},{"archiveTextureCount",0},{"archiveSamplerCount",0},
      {"evaluatedImageCount",0},{"evaluatedTextureCount",0},{"evaluatedSamplerCount",0},{"runtimeTextureCount",0},
      {"resourcesTruncated",false},{"textures",nlohmann::json::array()},{"slots",nlohmann::json::array()},
      {"runtimeResources",nlohmann::json::array()},{"evidence","CPU source/resource metadata, not GPU buffer readback"}};
  auto& resourceCatalog=json["gltfResourceCatalog"];
  resourceCatalog={{"assetIdentity",0},{"resourceRevision",0},{"generationRevision",0},{"generationEpoch",0},
      {"geometryRevision",0},{"materialRevision",0},{"authoredMaterialRevision",0},{"rows",nlohmann::json::array()},
      {"runtimeRows",nlohmann::json::array()},{"actualImages",nlohmann::json::array()},{"rowsTruncated",false},{"truncated",false},
      {"activeRows",0},{"retiredRows",0},{"runtimeSlotCount",0},{"activeRuntimeCount",0},{"coveredActualImageCount",0},
      {"historyReservationBytes",materialBindingBudget_?materialBindingBudget_->reserved:0},
      {"evidence","Logical phases and retained allocation capabilities; not GPU readback or allocator-release proof"}};
  if(!subs_.sceneManager) return;
  const auto& manager=*subs_.sceneManager;const auto archive=manager.authoredMaterialResources();
  const auto& evaluated=manager.animationSource();const auto& liveRoles=manager.gltfTextureResources();
  resourceCatalog["assetIdentity"]=manager.materialAssetIdentity();resourceCatalog["resourceRevision"]=manager.materialResourceRevision();
  resourceCatalog["generationEpoch"]=manager.generationEpoch();resourceCatalog["geometryRevision"]=manager.geometryRevision();
  resourceCatalog["materialRevision"]=manager.materialRevision();resourceCatalog["authoredMaterialRevision"]=manager.authoredMaterialRevision();
  resourceCatalog["runtimeSlotCount"]=manager.materialTextureResourceCount();resourceCatalog["lastError"]=resourceCatalogError_;
  const auto generation=manager.resourceLifecycleGeneration();
  if(generation) {
    resourceCatalog["generationRevision"]=generation->revision();resourceCatalog["logicalGenerationValid"]=generation->valid();
    resourceCatalog["opaqueReferenceOwners"]=generation->opaqueReferenceOwnerCount();
    using Domain=container::util::ResourceDomainKind;using Kind=container::util::ResourceKind;
    uint64_t activeRows=0,retiredRows=0,activeRuntime=0,work=0;
    for(const auto [domain,name]:{std::pair{Domain::Archive,"archive"},std::pair{Domain::Evaluated,"evaluated"},std::pair{Domain::Runtime,"runtime"}})
      for(const auto [kind,kindName]:{std::pair{Kind::Image,"image"},std::pair{Kind::Texture,"texture"},std::pair{Kind::Sampler,"sampler"},std::pair{Kind::RuntimeTexture,"runtimeTexture"}}) {
        if((domain==Domain::Runtime)!=(kind==Kind::RuntimeTexture)) continue;
        const auto count=generation->rowCount(domain,kind);
        if(count>1'000'000-work) {resourceCatalog["rowsTruncated"]=true;continue;}work+=count;
        for(uint32_t row=0;row<count;++row) {
          const container::scene::TextureResourceSelection selection{domain,kind,row};const auto handle=generation->resource(selection);
          const bool active=generation->active(selection);activeRows+=active;retiredRows+=!active;
          if(domain==Domain::Runtime) activeRuntime+=active;
          if(resourceCatalog["rows"].size()<65536) resourceCatalog["rows"].push_back({{"domain",name},{"kind",kindName},{"row",row},
              {"active",active},{"domainIdentity",handle?handle.domain().identity():0},{"incarnation",handle?handle.incarnation():0},
              {"resourceRevision",manager.materialResourceRevision()}});
          else resourceCatalog["rowsTruncated"]=true;
        }
      }
    resourceCatalog["activeRows"]=activeRows;resourceCatalog["retiredRows"]=retiredRows;resourceCatalog["activeRuntimeCount"]=activeRuntime;
  }
  if(const auto ownership=manager.textureImageOwnership()) {
    resourceCatalog["coveredActualImageCount"]=ownership->imageCount();resourceCatalog["imageOwnerComplete"]=ownership->complete();
    resourceCatalog["imageOwnerRevision"]=ownership->revision();resourceCatalog["imageOwnerBytes"]=ownership->ownedBytes();
    resourceCatalog["retainedImagePinControls"]=ownership->pins().size();
    for(size_t image=0;image<std::min<size_t>(ownership->imageCount(),65536);++image) {
      const auto& capability=ownership->images()[image];container::material::TextureResource alias;
      alias.allocationIdentity=capability.identity;alias.image=capability.image;alias.imageView=capability.imageView;
      resourceCatalog["actualImages"].push_back({{"image",image},{"allocationIncarnation",capability.identity.incarnation()},
          {"capabilityValid",svc_.allocationManager.textureImageMatches(alias)}});
    }
    for(const auto& row:ownership->rows()) {
      if(resourceCatalog["runtimeRows"].size()==65536) {resourceCatalog["rowsTruncated"]=true;break;}
      const bool active=row.image<ownership->images().size();
      resourceCatalog["runtimeRows"].push_back({{"row",row.runtimeSlot},{"active",active},
          {"image",active?nlohmann::json(row.image):nlohmann::json(nullptr)},
          {"allocationIncarnation",active?ownership->images()[row.image].identity.incarnation():0}});
    }
  }
  if(subs_.guiManager) {
    const auto& controls=subs_.guiManager->resourceCatalogSnapshot();resourceCatalog["retirementAvailable"]=controls.retirementAvailable;
    resourceCatalog["retirementReason"]=controls.retirementReason;
  }
  resourceCatalog["truncated"]=resourceCatalog["rowsTruncated"];
  result["resourceRevision"]=manager.materialResourceRevision();
  result["archiveImageCount"]=archive.images.size();result["archiveTextureCount"]=archive.textures.size();
  result["archiveSamplerCount"]=archive.samplers.size();result["evaluatedImageCount"]=evaluated.images.size();
  result["evaluatedTextureCount"]=evaluated.textures.size();result["evaluatedSamplerCount"]=evaluated.samplers.size();
  result["runtimeTextureCount"]=manager.materialTextureResourceCount();
  result["lastError"]=materialTextureErrorAsset_==manager.materialAssetIdentity()?materialTextureError_:std::string{};
  result["errorSourceMaterial"]=materialTextureErrorAsset_==manager.materialAssetIdentity()?materialTextureErrorSource_:-1;
  constexpr size_t maximumRows=256;
  result["resourcesTruncated"]=archive.textures.size()>maximumRows || manager.sourceMaterialRuntimeMapping().size()>maximumRows;
  const auto nullable=[](uint32_t value)->nlohmann::json {return value==UINT32_MAX?nlohmann::json(nullptr):nlohmann::json(value);};
  for(size_t i=0;i<std::min(archive.textures.size(),maximumRows);++i) {
    const auto& texture=archive.textures[i];
    int minFilter=-1,magFilter=-1,wrapS=10497,wrapT=10497;
    if(texture.sampler>=0 && size_t(texture.sampler)<archive.samplers.size()) {
      const auto& sampler=archive.samplers[size_t(texture.sampler)];
      minFilter=sampler.minFilter;magFilter=sampler.magFilter;wrapS=sampler.wrapS;wrapT=sampler.wrapT;
    }
    result["textures"].push_back({{"sourceTexture",i},
        {"evaluatedTexture",i<archive.archiveToEvaluated.size()?archive.archiveToEvaluated[i]:-1},
        {"image",materialTextureImage(texture)},{"sampler",texture.sampler},
        {"minFilter",minFilter},{"magFilter",magFilter},{"wrapS",wrapS},{"wrapT",wrapT},
        {"colorResource",nullable(i<archive.colorResources.size()?archive.colorResources[i]:UINT32_MAX)},
        {"dataResource",nullable(i<archive.dataResources.size()?archive.dataResources[i]:UINT32_MAX)}});
  }
  const auto mapping=manager.sourceMaterialRuntimeMapping();
  for(size_t source=0;source<std::min(mapping.size(),maximumRows);++source) {
    try {
      const auto& authored=manager.authoredMaterial(int32_t(source));
      const auto& material=evaluated.materials.at(source);
      for(const auto& info:GltfMaterialTextureSlotRegistry()) {
        const auto a=ReadGltfMaterialTextureBinding(authored,info.slot),e=ReadGltfMaterialTextureBinding(material,info.slot);
        const bool colorResource=info.srgb || e.coupledSlot.has_value();
        const auto& roles=colorResource?liveRoles.color:liveRoles.data;
        const auto runtime=e.index>=0 && size_t(e.index)<roles.size()?roles[size_t(e.index)]:UINT32_MAX;
        result["slots"].push_back({{"sourceMaterial",source},{"runtimeMaterial",mapping[source]},
            {"active",manager.materialIsActive(mapping[source])},
            {"slot",container::material::MaterialTextureSlotName(info.slot)},
            {"authoredTexture",a.index},{"evaluatedTexture",e.index},{"runtimeTexture",nullable(runtime)},
            {"texCoord",e.texCoord},{"channel",e.channel},{"offset",e.offset},{"scale",e.scale},
            {"rotation",e.rotation},{"srgb",colorResource}});
      }
    } catch(const std::exception&) {result["resourcesTruncated"]=true;}
  }
  const auto metadata=manager.materialTextureMetadata();
  if(metadata.size()>65536) result["resourcesTruncated"]=true;
  else for(size_t i=0;i<metadata.size();++i)
    result["runtimeResources"].push_back({{"runtimeTexture",i},{"samplerSlot",metadata[i].samplerIndex},
        {"samplerCount",metadata[i].samplerCount}});
}

void RendererFrontend::appendMaterialAuthoringTelemetry(nlohmann::json& json) const {
  appendMaterialTextureTelemetry(json);
  auto& bindings=json["gltfMaterialBindings"];
  bindings={{"sourceMaterialCount",subs_.sceneManager?subs_.sceneManager->sourceMaterialRuntimeMapping().size():0},
      {"runtimeMaterialCount",subs_.sceneManager?subs_.sceneManager->materialCount():0},
      {"activeRuntimeMaterialCount",0},{"retiredRuntimeMaterials",nlohmann::json::array()},
      {"runtimeMaterialStates",nlohmann::json::array()},{"runtimeMaterialStatesTruncated",false},
      {"sourceToRuntime",nlohmann::json::array()},{"materials",nlohmann::json::array()},{"materialsTruncated",false},
      {"draws",nlohmann::json::array()},{"drawsTruncated",false}};
  if(subs_.sceneManager) {
    const auto& manager=*subs_.sceneManager;const auto mapping=manager.sourceMaterialRuntimeMapping();
    if(manager.materialCount()>1'000'000) bindings["runtimeMaterialStatesTruncated"]=true;
    else {
      std::vector<int32_t> sourceIds(manager.materialCount(),-1);
      for(size_t source=0;source<mapping.size();++source) if(mapping[source]<sourceIds.size()) sourceIds[mapping[source]]=int32_t(source);
      uint64_t activeCount=0;
      for(uint32_t runtime=0;runtime<manager.materialCount();++runtime) {
        const bool active=manager.materialIsActive(runtime);activeCount+=active;
        if(runtime<65536) {
          const auto facts=manager.materialRenderProperties(runtime);
          bindings["runtimeMaterialStates"].push_back({{"runtimeMaterial",runtime},{"sourceMaterial",sourceIds[runtime]},
              {"active",active},{"incarnation",manager.materialIncarnation(runtime)},
              {"uploadedMaterialHash",manager.temporalMaterialRevision(runtime)},
              {"materialProperties",{{"transparent",facts.transparent},{"alphaMasked",facts.alphaMasked},
                  {"doubleSided",facts.doubleSided},{"unlit",facts.unlit},{"specularGlossiness",facts.specularGlossiness},{"heightScale",facts.heightScale}}}});
          if(!active) bindings["retiredRuntimeMaterials"].push_back(runtime);
        } else bindings["runtimeMaterialStatesTruncated"]=true;
      }
      bindings["activeRuntimeMaterialCount"]=activeCount;
    }
    const auto count=std::min<size_t>(mapping.size(),256);bindings["materialsTruncated"]=count!=mapping.size();
    uint64_t remaining=262144;
    for(size_t i=0;i<count;++i) {
      const auto runtime=mapping[i];const auto facts=manager.materialRenderProperties(runtime);
      try {
      nlohmann::json record={{"sourceMaterial",i},{"runtimeMaterial",runtime},{"active",manager.materialIsActive(runtime)},
          {"incarnation",manager.materialIncarnation(runtime)},
          {"authored",materialRecordJson(manager.authoredMaterial(int32_t(i)),remaining)},
          {"evaluated",materialRecordJson(manager.animationSource().materials.at(i),remaining)},
          {"uploadedMaterialHash",manager.temporalMaterialRevision(runtime)},
          {"materialProperties",{{"transparent",facts.transparent},{"alphaMasked",facts.alphaMasked},
              {"doubleSided",facts.doubleSided},{"unlit",facts.unlit},{"specularGlossiness",facts.specularGlossiness},{"heightScale",facts.heightScale}}}};
      bindings["sourceToRuntime"].push_back(runtime);bindings["materials"].push_back(std::move(record));
      } catch(const std::runtime_error& error) {
        if(std::string_view(error.what())!="Material telemetry metadata exceeds its limits") throw;
        bindings["materialsTruncated"]=true;break;
      }
    }
  }
  if(subs_.sceneController) {
    const auto& controller=*subs_.sceneController;const auto& objects=controller.objectData();
    const std::array<std::span<const DrawCommand>,6> categories{
        controller.opaqueSingleSidedDrawCommands(),controller.opaqueWindingFlippedDrawCommands(),controller.opaqueDoubleSidedDrawCommands(),
        controller.transparentSingleSidedDrawCommands(),controller.transparentWindingFlippedDrawCommands(),controller.transparentDoubleSidedDrawCommands()};
    const std::array<std::span<const DrawCommand>,2> draws{controller.opaqueDrawCommands(),controller.transparentDrawCommands()};
    bool bounded=objects.size()<=65536;
    for(const auto list:categories) bounded=bounded && list.size()<=65536;
    for(const auto list:draws) bounded=bounded && list.size()<=65536;
    if(bounded) {
      uint64_t work=524288;std::vector<uint8_t> route(objects.size()),seen(objects.size());
      const auto visit=[&](std::span<const DrawCommand> commands,const auto& callback) {
        for(const auto& draw:commands) {
          if(!work || draw.objectIndex>=objects.size() || !draw.instanceCount || draw.instanceCount>objects.size()-draw.objectIndex ||
              uint64_t(draw.instanceCount)+1>work) return false;
          work-=uint64_t(draw.instanceCount)+1;
          for(uint32_t i=0;i<draw.instanceCount;++i) if(!callback(draw.objectIndex+i,draw)) return false;
        }
        return true;
      };
      for(size_t category=0;bounded && category<categories.size();++category)
        bounded=visit(categories[category],[&](uint32_t object,const DrawCommand&) {
          if(route[object]) return false;route[object]=uint8_t(category+1);return true;
        });
      for(size_t alpha=0;bounded && alpha<draws.size();++alpha)
        bounded=visit(draws[alpha],[&](uint32_t object,const DrawCommand& draw) {
          const auto category=route[object];
          if(seen[object] || !category || bool(category>3)!=bool(alpha)) return false;
          seen[object]=1;const auto& data=objects[object];
          const bool sided=(category-1)%3==2;
          bindings["draws"].push_back({{"objectIndex",object},{"graphNode",data.temporalInfo.z},{"primitiveIndex",data.temporalInfo.w},
              {"runtimeMaterial",data.objectInfo.x},{"transparent",bool(alpha)},{"doubleSided",sided},
              {"objectDoubleSided",bool(data.objectInfo.y&container::gpu::kObjectFlagDoubleSided)},
              {"windingFlipped",(category-1)%3==1},{"firstIndex",draw.firstIndex},{"indexCount",draw.indexCount}});
          return true;
        });
      for(size_t i=0;bounded && i<objects.size();++i)
        if(objects[i].temporalInfo.w!=UINT32_MAX && !seen[i]) bounded=false;
    }
    if(!bounded) {bindings["drawsTruncated"]=true;bindings["draws"]=nlohmann::json::array();}
  }
  auto& view=json["gltfMaterialAuthoring"];
  view={{"publications",materialPublications_},{"pendingGesture",bool(materialGesture_)},
      {"sourceMaterial",lastEditedSourceMaterial_},{"runtimeMaterial",UINT32_MAX},
      {"materialRevision",subs_.sceneManager?subs_.sceneManager->materialRevision():0},
      {"generationEpoch",subs_.sceneManager?subs_.sceneManager->generationEpoch():0}};
  if(!subs_.sceneManager || lastEditedSourceMaterial_<0) return;
  const auto& manager=*subs_.sceneManager;const auto source=size_t(lastEditedSourceMaterial_);
  const auto mapping=manager.sourceMaterialRuntimeMapping();
  if(source>=mapping.size() || source>=manager.animationSource().materials.size()) return;
  const auto runtime=mapping[source];const auto properties=manager.materialRenderProperties(runtime);
  view["active"]=manager.materialIsActive(runtime);view["incarnation"]=manager.materialIncarnation(runtime);
  view["runtimeMaterial"]=runtime;view["authored"]=materialRecordJson(manager.authoredMaterial(lastEditedSourceMaterial_));
  view["evaluated"]=materialRecordJson(manager.animationSource().materials[source]);
  view["uploadedMaterialHash"]=manager.temporalMaterialRevision(runtime);
  view["materialProperties"]={{"transparent",properties.transparent},{"alphaMasked",properties.alphaMasked},
      {"doubleSided",properties.doubleSided},{"unlit",properties.unlit},{"specularGlossiness",properties.specularGlossiness},
      {"heightScale",properties.heightScale}};
}
} // namespace container::renderer
