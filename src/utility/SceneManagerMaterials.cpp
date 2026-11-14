#include <Container/utility/SceneManager.h>
#include <Container/utility/GltfMaterialConversion.h>
#include <Container/utility/GltfRuntimeMaterialAuthoring.h>
#include <Container/utility/PipelineManager.h>
#include <Container/utility/SourceMaterialMapping.h>
#include <Container/utility/VulkanDevice.h>
#include <Container/utility/VulkanMemoryManager.h>
#include <Container/utility/TextureResourceLifecycle.h>
#include <Container/geometry/GltfTextureResourceReferences.h>
#include <Container/geometry/GltfExtendedMaterials.h>
#include <Container/renderer/temporal/TemporalMaterialRevision.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <bit>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace container::scene {
namespace {
using namespace container::geometry::gltf;
[[noreturn]] void invalid(const char* message) { throw std::invalid_argument(message); }
constexpr size_t experimentalTextureSlots = 3;

template<class Visit>
void visitExperimentalMaterialTextures(const tinygltf::Material& record,
    std::span<const std::string> declarations, size_t textureCount, Visit visit) {
  const auto layer=ParseGltfExperimentalMaterial(record,declarations,textureCount);
  for(const auto& [texture,color]:std::array{
      std::pair{layer.retroreflectionTexture.sourceTexture,false},
      std::pair{layer.scatterStrengthTexture.sourceTexture,false},
      std::pair{layer.scatterColorTexture.sourceTexture,true}})
    if(texture>=0) visit(uint32_t(texture),color);
}
struct Budget {
  uint64_t available;
  void add(uint64_t count,uint64_t width=1) {
    if(width && count>available/width) invalid("Material generation exceeds its CPU budget");
    available-=count*width;
  }
};
struct MaterialResourceSelections {
  Budget& budget;
  std::vector<TextureResourceSelection> rows;
  bool opaque{};
  uint64_t storageBytes{};
  void add(container::util::ResourceDomainKind domain,container::util::ResourceKind kind,uint32_t row) {
    if(row==UINT32_MAX) return;
    if(rows.size()==rows.capacity()) {
      const auto next=std::max<size_t>(8,rows.size()*2);
      if(next<rows.size() || next>1'000'000) invalid("Selected material resource references exceed their row capacity");
      const auto bytes=uint64_t(next-rows.capacity())*sizeof(TextureResourceSelection);
      budget.add(bytes);storageBytes+=bytes;rows.reserve(next);
    }
    rows.push_back({domain,kind,row});
  }
  void runtime(std::span<const uint32_t> slots) {
    for(const auto slot:slots) add(container::util::ResourceDomainKind::Runtime,container::util::ResourceKind::RuntimeTexture,slot);
  }
  void requireActive(const TextureResourceLifecycleGenerationPtr& generation) const {
    if(!generation) invalid("Selected material resources have no logical generation");
    for(const auto selection:rows) if(!generation->active(selection))
      invalid("Selected material resources reference a missing or retired incarnation");
  }
  void record(const tinygltf::Material& material,container::util::ResourceDomainKind domain,
      const TextureResourceLifecycleGenerationPtr& generation) {
    if(!generation || !budget.available) invalid("Selected material resources have no logical generation or traversal budget");
    GltfTextureResourceReferenceLimits limits;limits.maxOwnedBytes=std::min(limits.maxOwnedBytes,budget.available);
    limits.policy=GltfTextureResourceReferencePolicy::RecordIncomplete;
    const auto proof=ExtractGltfMaterialRawTextureReferences(material,
        generation->rowCount(domain,container::util::ResourceKind::Texture),{},limits);
    budget.add(proof.stats().ownedBytes);opaque=opaque || !proof.complete();
    // Checking only selected rows avoids an O(source table) mask per material.
    // The generation's source Texture proof covers all Image/Sampler targets.
    for(const auto texture:proof.textures()) {
      if(!generation->active({domain,container::util::ResourceKind::Texture,texture}))
        invalid("Selected material source references a retired texture");
      add(domain,container::util::ResourceKind::Texture,texture);
    }
  }
};
int32_t importedExportTextureIndex(const tinygltf::Material& source,GltfMaterialTextureSlot slot) {
  using Slot=GltfMaterialTextureSlot;
  switch(slot) {
    case Slot::BaseColor:return source.pbrMetallicRoughness.baseColorTexture.index;
    case Slot::Normal:return source.normalTexture.index;
    case Slot::Occlusion:return source.occlusionTexture.index;
    case Slot::Emissive:return source.emissiveTexture.index;
    case Slot::MetallicRoughness:return source.pbrMetallicRoughness.metallicRoughnessTexture.index;
    default:break;
  }
  const auto& info=GltfMaterialTextureSlotDescription(slot);if(info.extension.empty()) return -1;
  const auto extension=source.extensions.find(std::string(info.extension));
  if(extension==source.extensions.end() || !extension->second.IsObject()) return -1;
  const auto field=std::string(info.path.substr(info.path.rfind('/')+1));
  const auto& texture=extension->second.Get(field);
  return texture.IsObject() && texture.Get("index").IsInt()?texture.Get("index").Get<int>():-1;
}
MaterialRenderProperties properties(uint32_t id,const container::material::Material& material) {
  return {id,material.alphaMode==container::material::AlphaMode::Blend || material.transmissionFactor>0.f,
      material.alphaMode==container::material::AlphaMode::Mask,material.doubleSided,
      material.specularGlossinessWorkflow,material.unlit,material.heightScale};
}
bool sameRouting(const MaterialRenderProperties& a,const MaterialRenderProperties& b) {
  return a.gpuMaterialIndex==b.gpuMaterialIndex && a.transparent==b.transparent && a.alphaMasked==b.alphaMasked &&
      a.doubleSided==b.doubleSided && a.specularGlossiness==b.specularGlossiness && a.unlit==b.unlit &&
      std::bit_cast<uint32_t>(a.heightScale)==std::bit_cast<uint32_t>(b.heightScale);
}
std::vector<int32_t> materialSourceOwners(std::span<const uint32_t> mapping,size_t count,
    const MaterialGenerationLimits& limits) {
  if(count>limits.maxMaterialCount || count>limits.references.maxOwnedBytes/sizeof(int32_t))
    invalid("Private material source-cache proof exceeds its row budget");
  std::vector<int32_t> result(count,-1);
  for(size_t source=0;source<mapping.size();++source) {
    if(mapping[source]>=count || source>size_t(INT32_MAX)) invalid("Private material cache mapping is invalid");
    auto& owner=result[mapping[source]];owner=owner==-1?int32_t(source):-2;
  }
  return result;
}
// No source models or historical prefixes are copied. The private source cache
// is a duplicate A record for source-backed rows. Runtime-only records have no
// proved material-index namespace, so normative LOD/unknown schemas fail closed.
struct MaterialCacheScan {
  const MaterialTailReferenceLimits& limits;
  uint64_t bytes{},values{},steps{};
  bool work(uint64_t n=1) noexcept {if(n>limits.maxTraversalSteps-steps) return false;steps+=n;return true;}
  bool text(std::string_view value) noexcept {
    if(value.size()>limits.maxMetadataBytes-bytes) return false;bytes+=value.size();return work();
  }
  static bool materialExtension(std::string_view name) noexcept {
    for(const auto known:{"KHR_materials_unlit","KHR_materials_emissive_strength","KHR_materials_ior","KHR_materials_dispersion",
        "KHR_materials_transmission","KHR_materials_volume","KHR_materials_clearcoat","KHR_materials_sheen",
        "KHR_materials_specular","KHR_materials_iridescence","KHR_materials_anisotropy",
        "KHR_materials_diffuse_transmission","KHR_materials_pbrSpecularGlossiness"}) if(name==known) return true;
    return false;
  }
  bool value(const tinygltf::Value& root,bool inspect) noexcept {
    using V=tinygltf::Value;using O=V::Object;
    struct Frame {const V* value{};O::const_iterator member;size_t next{};bool entered{},inspect{};};
    std::array<Frame,65> stack;size_t depth{};stack[0].value=&root;stack[0].inspect=inspect;
    for(;;) {
      auto& frame=stack[depth];const auto& v=*frame.value;
      if(!frame.entered) {
        if(!work() || ++values>limits.maxMetadataValues || sizeof(V)>limits.maxMetadataBytes-bytes) return false;
        bytes+=sizeof(V);
        if((!v.IsString() && !v.Get<std::string>().empty()) || (!v.IsArray() && !v.Get<V::Array>().empty()) ||
            (!v.IsObject() && !v.Get<O>().empty()) || (!v.IsBinary() && !v.Get<std::vector<unsigned char>>().empty())) return false;
        if(v.IsString() && !text(v.Get<std::string>())) return false;
        if(v.IsNumber() && !std::isfinite(v.GetNumberAsDouble())) return false;
        if(v.IsBinary()) {if(v.Get<std::vector<unsigned char>>().size()>limits.maxMetadataBytes-bytes) return false;bytes+=v.Get<std::vector<unsigned char>>().size();}
        frame.entered=true;if(v.IsObject()) frame.member=v.Get<O>().begin();
      }
      const V* child{};bool inspectChild=frame.inspect;
      if(v.IsArray() && frame.next<v.Get<V::Array>().size()) child=&v.Get<V::Array>()[frame.next++];
      else if(v.IsObject() && frame.member!=v.Get<O>().end()) {
        const auto& [key,next]=*frame.member++;if(!text(key)) return false;child=&next;
        if(key=="extras") inspectChild=false;
        else if(frame.inspect && key=="extensions") {
          if(!next.IsObject()) return false;
          for(const auto& [name,body]:next.Get<O>())
            if(!work() || name!="KHR_texture_transform" || !body.IsObject()) return false;
        }
      }
      if(child) {
        if(depth==limits.maxMetadataDepth || depth==64) return false;
        auto& next=stack[++depth];next.value=child;next.entered=false;next.next=0;next.inspect=inspectChild;
      } else if(!depth) return true;else --depth;
    }
  }
  template<class T> bool record(const T& record,bool sourceBacked,bool texture=false) noexcept {
    if(!work()) return false;
    if constexpr(requires {record.name;}) if(!text(record.name)) return false;
    if(!value(record.extras,false) || !text(record.extras_json_string) || !text(record.extensions_json_string)) return false;
    for(const auto& [name,body]:record.extensions) {
      if(!text(name) || (!sourceBacked && !(texture?name=="KHR_texture_transform":materialExtension(name))) ||
          !value(body,!sourceBacked)) return false;
    }
    return true;
  }
};
bool privateMaterialCacheSafe(const std::vector<std::optional<tinygltf::Material>>& cache,
    const tinygltf::Model& authored,std::span<const uint32_t> mapping,std::span<const int32_t> owners,
    uint32_t removed,const MaterialTailReferenceLimits& limits) noexcept {
  if(cache.size()>owners.size() || authored.materials.size()!=mapping.size() || owners.size()>limits.maxTableEntries ||
      !limits.maxMetadataDepth || limits.maxMetadataDepth>64) return false;
  for(size_t source=0;source<mapping.size();++source)
    if(mapping[source]>=owners.size() || owners[mapping[source]]!=int32_t(source)) return false;
  MaterialCacheScan scan{limits};
  for(size_t runtime=0;runtime<cache.size();++runtime) if(runtime!=removed && cache[runtime]) {
    const auto& m=*cache[runtime];const auto source=owners[runtime];const bool backed=source>=0;
    if(source==-2 || (backed && size_t(source)>=authored.materials.size()) ||
        !scan.record(m,backed) || !scan.record(m.pbrMetallicRoughness,backed) ||
        !scan.record(m.pbrMetallicRoughness.baseColorTexture,backed,true) ||
        !scan.record(m.pbrMetallicRoughness.metallicRoughnessTexture,backed,true) ||
        !scan.record(m.normalTexture,backed,true) || !scan.record(m.occlusionTexture,backed,true) ||
        !scan.record(m.emissiveTexture,backed,true)) return false;
    for(const auto* numbers:{&m.lods})
      if(numbers->size()>limits.maxTableEntries || !scan.work(numbers->size())) return false;
    for(const auto* numbers:{&m.emissiveFactor,&m.pbrMetallicRoughness.baseColorFactor})
      if(numbers->size()>limits.maxTableEntries || !scan.work(numbers->size())) return false;
    for(const auto* parameters:{&m.values,&m.additionalValues}) {
      if(parameters->size()>limits.maxTableEntries || !scan.work(parameters->size())) return false;
      for(const auto& [name,p]:*parameters) {
        if(!scan.text(name) || !scan.text(p.string_value) || p.number_array.size()>limits.maxTableEntries ||
            p.json_double_value.size()>limits.maxTableEntries || !scan.work(p.number_array.size()) ||
            !scan.work(p.json_double_value.size())) return false;
        for(const auto& [key,number]:p.json_double_value) if(!scan.text(key) || !std::isfinite(number)) return false;
      }
    }
    if(backed) {if(!GltfMaterialRecordsEqual(m,authored.materials[size_t(source)])) return false;}
    else if(!m.lods.empty() || !m.pbrMetallicRoughness.extensions.empty()) return false;
  }
  return true;
}
}

struct SceneManager::MaterialResourceDependencyState {
  uint32_t runtime{};
  int32_t source{-1};
  container::util::ResourceHandle material;
  TextureResourceLiveOwnerPtr live;
  container::material::Material value;
  std::array<GltfMaterialRawTextureReferences,4> records;
  std::array<size_t,4> textureCounts{};
  std::array<uint32_t,23> runtimeBindings{};
  std::array<uint32_t,23> fallbackRuntime{};
  std::array<int32_t,23> fallbackTextures{};
  std::array<bool,23> fallbackResolved{};
  std::optional<ImportedMaterialTextureBindings> exportBindings;
  std::optional<RuntimeMaterialProvenance> provenance;
  std::optional<tinygltf::Material> rest;
  uint64_t bytes{};
  bool matches(const MaterialResourceDependencyInput& input,
      const TextureResourceLifecycleGenerationPtr& generation,std::span<const int32_t> exportMapping,bool prospective=false) const noexcept {
    if(!generation || !live || !live->valid() || !(prospective?(live->pending() || live->active()):live->active()) || !live->matches(generation) ||
        runtime!=input.runtime || (input.source!=INT32_MIN && source!=input.source) || material!=input.material || live->key().owner!=material ||
        live->key().kind!=TextureResourceLiveOwnerKind::Material || !input.value ||
        !container::material::RuntimeMaterialsEqual(value,*input.value) ||
        snapshotMaterialTextureBindings(*input.value).runtimeIndices!=runtimeBindings ||
        bool(exportBindings)!=bool(input.exportBindings) || bool(provenance)!=bool(input.provenance)) return false;
    if(exportBindings && exportBindings->runtimeIndices!=input.exportBindings->runtimeIndices) return false;
    if(provenance && *provenance!=*input.provenance) return false;
    if(bool(rest)!=bool(input.rest)) return false;
    const std::array<const tinygltf::Material*,4> inputs{input.authored,input.evaluated,input.exported,input.rest};
    for(size_t i=0;i<records.size();++i) {
      if(bool(records[i])!=bool(inputs[i])) return false;
      if(inputs[i]) {
        if(!GltfMaterialRawTextureReferencesMatch(*inputs[i],textureCounts[i],{},records[i])) return false;
        const auto domain=i==1?container::util::ResourceDomainKind::Evaluated:container::util::ResourceDomainKind::Archive;
        for(const auto texture:records[i].textures())
          if(!generation->active({domain,container::util::ResourceKind::Texture,texture})) return false;
      }
    }
    for(size_t i=0;i<fallbackRuntime.size();++i) if(fallbackRuntime[i]!=UINT32_MAX) {
      const auto runtime=fallbackRuntime[i];const int32_t current=runtime<exportMapping.size()?exportMapping[runtime]:-1;
      const bool inRange=current>=0 && size_t(current)<generation->rowCount(
          container::util::ResourceDomainKind::Archive,container::util::ResourceKind::Texture);
      const bool active=inRange && generation->active({container::util::ResourceDomainKind::Archive,
          container::util::ResourceKind::Texture,uint32_t(current)});
      if(current!=fallbackTextures[i] || active!=fallbackResolved[i] || (inRange && !active)) return false;
    }
    if(rest && !GltfMaterialRecordsEqual(*rest,*input.rest)) return false;
    return true;
  }
};

SceneManager::MaterialResourceDependencyStatePtr SceneManager::stageMaterialResourceDependency(
    const MaterialResourceDependencyInput& input,PreparedTextureResourceLifecycle* mutation,
    PreparedMaterialResources* resources,const MaterialResourceDependencyStatePtr& outgoing,uint64_t maxCpuBytes) const try {
  using namespace container::util;
  if(!maxCpuBytes || maxCpuBytes==UINT64_MAX || !input.value || !input.material ||
      input.material.kind()!=ResourceKind::Material || bool(mutation)==bool(resources))
    invalid("Material live dependencies require an exact owner, one source mutation and finite budget");
  const auto generation=resources?resources->resourceLifecycleGeneration():mutation->next();
  const auto exportMapping=input.exportMappingOverride?*input.exportMappingOverride:
      (resources?resources->exportTextureIndices():std::span<const int32_t>(exportTextureIndices_));
  if(!generation || (resources && !canPrepareMaterialResources(*resources)))
    invalid("Material live dependencies require a coherent structural source candidate");
  if(outgoing && (outgoing->material!=input.material || !outgoing->live || !outgoing->live->active() ||
      outgoing->live->key().owner!=input.material || outgoing->live->key().kind!=TextureResourceLiveOwnerKind::Material))
    invalid("Material live dependency replacement has a stale or foreign outgoing owner");
  if(!resources && outgoing && outgoing->matches(input,generation,exportMapping) &&
      mutation->liveOwnerWillBeActive(outgoing->live)) return outgoing;
  Budget budget{maxCpuBytes};budget.add(sizeof(MaterialResourceDependencyState)+256);
  auto next=std::make_shared<MaterialResourceDependencyState>();next->runtime=input.runtime;next->material=input.material;
  next->value=*input.value;
  next->source=input.source;
  if(next->source==INT32_MIN) {
    next->source=-1;
    if(sourceMaterialToRuntime_.size()>1'000'000) invalid("Material dependency source mapping exceeds its capacity");
    for(size_t source=0;source<sourceMaterialToRuntime_.size();++source) if(sourceMaterialToRuntime_[source]==input.runtime) {
      if(next->source!=-1 || source>size_t(INT32_MAX)) invalid("Material dependency source ownership is ambiguous");
      next->source=int32_t(source);
    }
  }
  next->runtimeBindings=snapshotMaterialTextureBindings(*input.value).runtimeIndices;
  next->fallbackRuntime.fill(UINT32_MAX);next->fallbackTextures.fill(-1);
  if(input.exportBindings) next->exportBindings=*input.exportBindings;
  if(input.provenance) next->provenance=*input.provenance;
  MaterialResourceSelections selected{budget};selected.runtime(next->runtimeBindings);
  selected.runtime(input.value->experimental.textureResources());
  if(input.exportBindings) selected.runtime(input.exportBindings->runtimeIndices);
  const std::array<const tinygltf::Material*,4> records{input.authored,input.evaluated,input.exported,input.rest};
  for(size_t i=0;i<records.size();++i) if(records[i]) {
    GltfTextureResourceReferenceLimits limits;limits.policy=GltfTextureResourceReferencePolicy::RecordIncomplete;
    limits.maxOwnedBytes=std::min(limits.maxOwnedBytes,budget.available);
    if(!limits.maxOwnedBytes) invalid("Material live dependency source proofs exceed their budget");
    const auto domain=i==1?ResourceDomainKind::Evaluated:ResourceDomainKind::Archive;
    next->textureCounts[i]=generation->rowCount(domain,ResourceKind::Texture);
    next->records[i]=ExtractGltfMaterialRawTextureReferences(*records[i],next->textureCounts[i],{},limits);
    budget.add(next->records[i].stats().ownedBytes);selected.opaque|=!next->records[i].complete();
    for(const auto texture:next->records[i].textures()) {
      if(!generation->active({domain,ResourceKind::Texture,texture}))
        invalid("Material live source dependencies target a retired texture");
      selected.add(domain,ResourceKind::Texture,texture);
    }
  }
  if(input.rest) {
    GltfMaterialAuthoringLimits limits;limits.maxOutputBytes=std::min(limits.maxOutputBytes,budget.available);
    if(!limits.maxOutputBytes) invalid("Material rest dependency ownership exceeds its budget");
    budget.add(GltfMaterialRecordStorageBytes(*input.rest,limits));next->rest=*input.rest;
  }
  if(input.provenance && input.provenance->known) for(size_t i=0;i<input.provenance->slots.size();++i)
    if(const auto& reference=input.provenance->slots[i]) {
      if(!*reference || reference->runtimeResource()!=next->runtimeBindings[i])
        invalid("Material live dependency provenance disagrees with its runtime binding");
      selected.add(ResourceDomainKind::Archive,ResourceKind::Texture,reference->archiveTexture());
      if(reference->evaluatedTexture()) selected.add(ResourceDomainKind::Evaluated,ResourceKind::Texture,*reference->evaluatedTexture());
      selected.add(ResourceDomainKind::Runtime,ResourceKind::RuntimeTexture,reference->runtimeResource());
    }
  budget.add(4096); // Bounded selected-field/export lookup scratch.
  if(!(input.value->specularGlossinessWorkflow && next->source>=0)) for(const auto& info:GltfMaterialTextureSlotRegistry()) {
    const auto slot=size_t(info.slot);const auto runtime=next->runtimeBindings[slot];if(runtime==UINT32_MAX) continue;
    if(input.value->specularGlossinessWorkflow) {selected.opaque=true;continue;}
    if(input.value->unlit && slot>size_t(GltfMaterialTextureSlot::MetallicRoughness)) continue;
    const bool custom=info.slot==GltfMaterialTextureSlot::Roughness || info.slot==GltfMaterialTextureSlot::Metalness ||
        info.slot==GltfMaterialTextureSlot::Height || info.slot==GltfMaterialTextureSlot::Opacity;
    const auto binding=input.exported?ReadGltfMaterialTextureBinding(*input.exported,info.slot):GltfMaterialTextureBinding{};
    const bool proved=input.exported && input.exportBindings && input.exportBindings->runtimeIndices[slot]==runtime;
    if(custom || ((info.slot==GltfMaterialTextureSlot::Specular || info.slot==GltfMaterialTextureSlot::Transmission) && binding.legacy && binding.index>=0)) {
      if(!proved || !binding.legacy || binding.index<0) selected.opaque=true;
      continue; // Serializer preserves a proved legacy index, or rejects export.
    }
    if(proved && importedExportTextureIndex(*input.exported,info.slot)>=0) continue;
    const int32_t target=runtime<exportMapping.size()?exportMapping[runtime]:-1;
    next->fallbackRuntime[slot]=runtime;next->fallbackTextures[slot]=target;
    const bool inRange=target>=0 && size_t(target)<generation->rowCount(ResourceDomainKind::Archive,ResourceKind::Texture);
    next->fallbackResolved[slot]=inRange && generation->active({ResourceDomainKind::Archive,ResourceKind::Texture,uint32_t(target)});
    if(inRange && !next->fallbackResolved[slot]) invalid("Material export fallback targets a retired texture");
    if(next->fallbackResolved[slot]) selected.add(ResourceDomainKind::Archive,ResourceKind::Texture,uint32_t(target));
    else selected.opaque=true;
  }
  budget.available+=4096; // Selected-field lookup scratch has been released.
  selected.requireActive(generation);
  const TextureResourceLiveOwnerKey key{input.material,TextureResourceLiveOwnerKind::Material,0};
  next->live=resources?resources->stageLiveOwner(key,selected.rows,outgoing?outgoing->live:TextureResourceLiveOwnerPtr{},budget.available,selected.opaque):
      mutation->stageLiveOwner(key,selected.rows,outgoing?outgoing->live:TextureResourceLiveOwnerPtr{},budget.available,selected.opaque);
  budget.add(next->live->ownedBytes());budget.available+=selected.storageBytes;
  next->bytes=maxCpuBytes-budget.available;return next;
} catch(const std::length_error& error) {
  throw std::invalid_argument(error.what());
}

std::vector<SceneManager::MaterialResourceDependencyStatePtr> SceneManager::stageCurrentMaterialResourceDependencies(
    PreparedTextureResourceLifecycle& mutation,uint64_t maxCpuBytes,
    std::span<const MaterialResourceDependencyStatePtr> outgoingOverride,const AnimationPlayer* player,
    std::span<const RuntimeMaterialProvenance> provenanceOverride,uint32_t skipRuntime,
    std::optional<std::span<const int32_t>> exportMappingOverride) const try {
  using namespace container::util;
  const auto count=materialManager_.materialCount();
  if(!maxCpuBytes || maxCpuBytes==UINT64_MAX || count>1'000'000 || !materialLifecycle_ ||
      materialLifecycleHandles_.size()!=count || !mutation)
    invalid("Current material live dependencies require exact material identities and finite capacities");
  const auto outgoing=outgoingOverride.empty()?std::span<const MaterialResourceDependencyStatePtr>(materialResourceDependencies_):outgoingOverride;
  if(outgoing.size()>1'000'000) invalid("Outgoing material dependency controls exceed their row capacity");
  if(!provenanceOverride.empty() && provenanceOverride.size()!=count)
    invalid("Prospective material dependency provenance does not cover the current rows");
  Budget budget{maxCpuBytes};budget.add(count,sizeof(MaterialResourceDependencyStatePtr)+sizeof(int32_t));
  budget.add(outgoing.size(),sizeof(uint8_t));
  MaterialGenerationLimits limits;limits.references.maxOwnedBytes=budget.available;
  const auto owners=materialSourceOwners(sourceMaterialToRuntime_,count,limits);
  std::vector<MaterialResourceDependencyStatePtr> next(count);std::vector<uint8_t> replaced(outgoing.size());
  for(uint32_t runtime=0;runtime<count;++runtime) if(materialIsActive(runtime)) {
    if(runtime==skipRuntime) {
      if(runtime>=outgoing.size() || !outgoing[runtime] || outgoing[runtime]->material!=materialLifecycleHandles_[runtime] ||
          !outgoing[runtime]->live || !outgoing[runtime]->live->active())
        invalid("Paired material dependency preparation requires its current exact owner");
      next[runtime]=outgoing[runtime];replaced[runtime]=1;continue;
    }
    MaterialResourceDependencyInput input;input.runtime=runtime;input.material=materialLifecycleHandles_[runtime];
    input.exportMappingOverride=exportMappingOverride;
    input.value=materialManager_.getMaterial(runtime);const auto source=owners[runtime];input.source=source;
    if(source< -1) invalid("Material live dependency source ownership is ambiguous");
    if(source>=0) {
      if(size_t(source)>=exportSource_.materials.size() || size_t(source)>=gltfModel_.materials.size())
        invalid("Material live dependency source records are unavailable");
      input.authored=&exportSource_.materials[size_t(source)];input.evaluated=&gltfModel_.materials[size_t(source)];
      if(player) {
        if(size_t(source)>=player->library().restMaterials.size()) invalid("Material rest dependency row is unavailable");
        input.rest=&player->library().restMaterials[size_t(source)];
      } else if(runtime<materialResourceDependencies_.size() && materialResourceDependencies_[runtime] && materialResourceDependencies_[runtime]->rest)
        input.rest=&*materialResourceDependencies_[runtime]->rest;
      else input.rest=input.authored;
    }
    if(runtime<exportMaterialSources_.size() && exportMaterialSources_[runtime]) input.exported=&*exportMaterialSources_[runtime];
    if(runtime<exportMaterialTextureBindings_.size() && exportMaterialTextureBindings_[runtime]) input.exportBindings=&*exportMaterialTextureBindings_[runtime];
    if(!provenanceOverride.empty()) input.provenance=&provenanceOverride[runtime];
    else if(runtime<runtimeMaterialProvenance_.size()) input.provenance=&runtimeMaterialProvenance_[runtime];
    MaterialResourceDependencyStatePtr old;
    if(runtime<outgoing.size() && outgoing[runtime] && outgoing[runtime]->material==input.material) {
      old=outgoing[runtime];replaced[runtime]=1;
    }
    next[runtime]=stageMaterialResourceDependency(input,&mutation,nullptr,old,budget.available);
    if(next[runtime]!=old) budget.add(next[runtime]->bytes);
  }
  for(size_t i=0;i<outgoing.size();++i) if(outgoing[i] && !replaced[i]) {
    if(!outgoing[i]->live || !outgoing[i]->live->active()) invalid("Outgoing material dependency controls are no longer current");
    const std::array released{outgoing[i]->live};mutation.releaseLiveOwners(released);
  }
  return next;
} catch(const std::length_error& error) {
  throw std::invalid_argument(error.what());
}

bool SceneManager::materialResourceDependenciesValid(const AnimationPlayer* player) const noexcept {
  const auto generation=resourceLifecycleGeneration();
  return generation && generation->liveDependenciesValid() && generation->matches(textureResourceLifecycleView()) &&
      materialResourceDependencyMapMatches(materialResourceDependencies_,generation,player,false);
}

bool SceneManager::materialResourceDependencyMapMatches(std::span<const MaterialResourceDependencyStatePtr> map,
    const TextureResourceLifecycleGenerationPtr& generation,const AnimationPlayer* player,bool prospective,
    std::optional<std::span<const int32_t>> exportMappingOverride) const noexcept {
  const auto count=materialManager_.materialCount();
  if(!generation || !materialLifecycle_ || materialLifecycleHandles_.size()!=count || map.size()!=count ||
      count>1'000'000 || sourceMaterialToRuntime_.size()>1'000'000) return false;
  for(size_t source=0;source<sourceMaterialToRuntime_.size();++source) {
    const auto runtime=sourceMaterialToRuntime_[source];
    if(runtime>=count || (materialIsActive(runtime) && (!map[runtime] || map[runtime]->source!=int32_t(source)))) return false;
  }
  for(uint32_t runtime=0;runtime<count;++runtime) {
    const auto& state=map[runtime];
    if(!materialIsActive(runtime)) {if(state) return false;continue;}
    if(!state) return false;
    MaterialResourceDependencyInput input;input.runtime=runtime;input.material=materialLifecycleHandles_[runtime];input.value=materialManager_.getMaterial(runtime);
    const auto source=state->source;input.source=source;
    if(source>=0) {
      if(size_t(source)>=exportSource_.materials.size() || size_t(source)>=gltfModel_.materials.size() ||
          size_t(source)>=sourceMaterialToRuntime_.size() || sourceMaterialToRuntime_[size_t(source)]!=runtime) return false;
      input.authored=&exportSource_.materials[size_t(source)];input.evaluated=&gltfModel_.materials[size_t(source)];
      if(player) {if(size_t(source)>=player->library().restMaterials.size()) return false;input.rest=&player->library().restMaterials[size_t(source)];}
      else if(state->rest) input.rest=&*state->rest;
    }
    if(runtime<exportMaterialSources_.size() && exportMaterialSources_[runtime]) input.exported=&*exportMaterialSources_[runtime];
    if(runtime<exportMaterialTextureBindings_.size() && exportMaterialTextureBindings_[runtime]) input.exportBindings=&*exportMaterialTextureBindings_[runtime];
    if(runtime<runtimeMaterialProvenance_.size()) input.provenance=&runtimeMaterialProvenance_[runtime];
    if(!state->matches(input,generation,exportMappingOverride?*exportMappingOverride:std::span<const int32_t>(exportTextureIndices_),prospective)) return false;
  }
  return true;
}

bool SceneManager::materialResourceDependencyMapWillBeActive(std::span<const MaterialResourceDependencyStatePtr> map,
    const PreparedTextureResourceLifecycle& mutation) const noexcept {
  for(const auto& state:map) if(state && (!state->live || !mutation.liveOwnerWillBeActive(state->live))) return false;
  return true;
}

TextureResourceLiveOwnerPtr SceneManager::materialResourceLiveOwner(uint32_t runtime) const noexcept {
  return runtime<materialResourceDependencies_.size() && materialResourceDependencies_[runtime]?
      materialResourceDependencies_[runtime]->live:TextureResourceLiveOwnerPtr{};
}

uint64_t SceneManager::materialResourceDependencyBytes() const noexcept {
  if(materialResourceDependencies_.capacity()>UINT64_MAX/sizeof(MaterialResourceDependencyStatePtr)) return UINT64_MAX;
  uint64_t bytes=uint64_t(materialResourceDependencies_.capacity())*sizeof(MaterialResourceDependencyStatePtr);
  for(const auto& state:materialResourceDependencies_) if(state) {
    if(state->bytes>UINT64_MAX-bytes) return UINT64_MAX;bytes+=state->bytes;
  }
  return bytes;
}

uint64_t SceneManager::materialResourceDependencyMapBytes(std::span<const MaterialResourceDependencyStatePtr> next,
    std::span<const MaterialResourceDependencyStatePtr> prior) noexcept {
  if(next.size()>UINT64_MAX/sizeof(MaterialResourceDependencyStatePtr)) return UINT64_MAX;
  uint64_t bytes=uint64_t(next.size())*sizeof(MaterialResourceDependencyStatePtr);
  for(size_t i=0;i<next.size();++i) if(next[i] && (i>=prior.size() || next[i]!=prior[i])) {
    if(next[i]->bytes>UINT64_MAX-bytes) return UINT64_MAX;bytes+=next[i]->bytes;
  }
  return bytes;
}

struct SceneManager::MaterialAppendCommand::State {
  const SceneManager* owner{};
  std::shared_ptr<const uint8_t> ownerIdentity,cookie,childVacancy;
  uint64_t asset{},bytes{};
  int32_t source{};uint32_t runtime{};
  enum class Phase : uint8_t { Unpublished,Present,Detached } phase{Phase::Unpublished};
  tinygltf::Material authored,evaluated;
  std::vector<uint32_t> prefixMapping;
  container::material::TextureImageOwnershipPtr images;
  TextureResourceReferenceOwnerPtr references;
  std::shared_ptr<const TextureSamplingFallback> samplingFallback;
  RuntimeMaterialProvenance provenance;
  container::util::ResourceHandle lifecycleHandle;
  container::util::ResourcePin lifecycleHistory;
  container::util::ResourceAppendCommand lifecycleAppend;
};
SceneManager::MaterialAppendCommand::operator bool() const noexcept {return bool(state_);}
int32_t SceneManager::MaterialAppendCommand::sourceMaterial() const noexcept {return state_?state_->source:-1;}
uint32_t SceneManager::MaterialAppendCommand::runtimeMaterial() const noexcept {return state_?state_->runtime:UINT32_MAX;}
bool SceneManager::MaterialAppendCommand::published() const noexcept {
  return state_ && state_->phase!=State::Phase::Unpublished;
}
bool SceneManager::MaterialAppendCommand::present() const noexcept {return state_ && state_->phase==State::Phase::Present;}
uint64_t SceneManager::MaterialAppendCommand::ownedBytes() const noexcept {return state_?state_->bytes:0;}
container::material::TextureImageOwnershipPtr SceneManager::MaterialAppendCommand::textureImageOwnership() const noexcept {
  return state_?state_->images:nullptr;
}
TextureResourceReferenceOwnerPtr SceneManager::MaterialAppendCommand::resourceReferences() const noexcept {
  return state_?state_->references:nullptr;
}

struct SceneManager::MaterialRetirementCommand::State {
  const SceneManager* owner{};
  std::shared_ptr<const uint8_t> ownerIdentity;
  std::shared_ptr<container::util::ResourceLifecycleCatalog> catalog;
  container::util::ResourceHandle handle;
  container::util::ResourceRetirementCommand authority;
  container::util::ResourcePin history;
  uint64_t asset{},bytes{};
  int32_t source{-1};uint32_t runtime{};
  bool published{},retired{};
  tinygltf::Material authored,evaluated;
  std::optional<tinygltf::Material> exported;
  std::optional<ImportedMaterialTextureBindings> bindings;
  container::material::Material value;
  RuntimeMaterialProvenance provenance;
  container::material::TextureImageOwnershipPtr images;
  TextureResourceReferenceOwnerPtr references;
  std::shared_ptr<const TextureSamplingFallback> samplingFallback;
};
SceneManager::MaterialRetirementCommand::operator bool() const noexcept {return bool(state_);}
int32_t SceneManager::MaterialRetirementCommand::sourceMaterial() const noexcept {return state_?state_->source:-1;}
uint32_t SceneManager::MaterialRetirementCommand::runtimeMaterial() const noexcept {return state_?state_->runtime:UINT32_MAX;}
bool SceneManager::MaterialRetirementCommand::published() const noexcept {return state_ && state_->published;}
bool SceneManager::MaterialRetirementCommand::retired() const noexcept {return state_ && state_->retired;}
uint64_t SceneManager::MaterialRetirementCommand::ownedBytes() const noexcept {return state_?state_->bytes:0;}
container::material::TextureImageOwnershipPtr SceneManager::MaterialRetirementCommand::textureImageOwnership() const noexcept {
  return state_?state_->images:nullptr;
}
TextureResourceReferenceOwnerPtr SceneManager::MaterialRetirementCommand::resourceReferences() const noexcept {
  return state_?state_->references:nullptr;
}

struct SceneManager::PreparedMaterialGeneration::State {
  struct AppendedTables {
    std::vector<tinygltf::Material> authored,evaluated;
    std::vector<std::optional<tinygltf::Material>> exported;
    std::vector<std::optional<ImportedMaterialTextureBindings>> bindings;
    std::vector<uint32_t> mapping;
    std::vector<std::shared_ptr<const uint8_t>> owners;
    container::material::MaterialManager runtime;
    std::optional<MaterialVariantLibrary> variants;
    size_t exportedBefore{},bindingsBefore{};
  };
  const SceneManager* owner{};
  std::shared_ptr<const uint8_t> ownerIdentity;
  std::shared_ptr<container::gpu::VulkanDevice> device;
  container::gpu::AllocationManager* allocation{};
  container::gpu::PipelineManager* pipelines{};
  MaterialGenerationLimits limits;
  GltfMaterialAuthoringPlan plan;
  MaterialAppendCommand command;
  MaterialRetirementCommand retirement;
  MaterialRetirementState retirementTarget{MaterialRetirementState::Active};
  std::shared_ptr<container::util::ResourceLifecycleCatalog> lifecycle;
  std::optional<container::util::PreparedResourceRegistration> lifecycleRegistration;
  std::optional<container::util::PreparedResourceTransition> lifecycleTransition;
  container::util::ResourcePin lifecyclePreparation;
  std::vector<container::util::ResourceHandle> lifecycleHandles;
  std::vector<int32_t> cacheSources;
  uint32_t lifecycleNextBefore{},lifecycleNextAfter{};
  std::optional<PreparedMaterialResources> resources;
  bool replay{},removal{};
  const SceneGraph* referenceGraph{};
  std::optional<MaterialTailReferenceAudit> audit;
  std::vector<uint32_t> protectedMaterials;
  std::shared_ptr<const uint8_t> expectedVacancy,futureVacancy;
  size_t ownersBefore{};
  size_t provenanceBefore{};
  std::shared_ptr<const uint8_t> resourceOwner;
  std::vector<RuntimeMaterialProvenance> provenance;
  std::vector<RuntimeMaterialProvenance> previousProvenanceRows;
  RuntimeMaterialProvenance previousProvenance;
  std::optional<std::pair<uint32_t,container::material::Material>> runtimeOrigin;
  // MSVC std::map move construction allocates a sentinel even when TinyGLTF
  // declares the enclosing move noexcept. Construct all swap scratch now.
  std::array<tinygltf::Material,3> scratch;
  tinygltf::Material evaluated;
  std::optional<tinygltf::Material> exportRecord;
  std::optional<ImportedMaterialTextureBindings> exportBindings;
  std::optional<tinygltf::Material> expectedExportRecord;
  std::optional<ImportedMaterialTextureBindings> expectedExportBindings;
  size_t exportRowsBefore{},bindingRowsBefore{};
  container::material::ConvertedGltfMaterial converted;
  std::unique_ptr<AppendedTables> appended;
  std::vector<container::gpu::GpuMaterial> gpu;
  std::vector<uint64_t> temporal;
  std::vector<MaterialRenderProperties> renderProperties;
  std::vector<std::string> authoredDeclarations,evaluatedDeclarations,expectedAuthoredDeclarations,expectedEvaluatedDeclarations;
  container::material::TextureImageOwnershipPtr beforeImages,images;
  TextureResourceLifecycleGenerationPtr beforeResourceGeneration,resourceGeneration;
  TextureResourceReferenceOwnerPtr beforeReferences,references;
  std::vector<MaterialResourceDependencyStatePtr> beforeDependencies,dependencies;
  std::optional<PreparedTextureResourceLifecycle> dependencyChange;
  std::shared_ptr<const TextureSamplingFallback> samplingFallback;
  container::material::TextureSamplingPlan sampling;
  std::optional<AnimationPlayer> player;
  MaterialAnimationCurveWitness curves;
  const AnimationPlayer* expectedPlayer{};
  std::optional<size_t> selectedClip;
  double time{},speed{};
  bool playing{},loop{},routing{},extended{},consumed{},descriptorsReady{},resourceTransition{};
  uint32_t runtime{};
  uint64_t epoch{},geometry{},material{},authoredRevision{},catalog{},resourceRevision{},descriptorCpuAvailable{};
  size_t materialCount{},sourceCount{},authoredTextureCount{},textureCount{},runtimeTextures{};
  VkDescriptorPool expectedPool{},pool{};
  bool ownsPool{true};
  VkDescriptorSetLayout layout{};
  VkBuffer expectedMaterial{},textureMetadata{},clip{};
  container::gpu::AllocatedBuffer buffer{};
  size_t capacity{};
  std::vector<VkDescriptorSet> primary,auxiliary;
  void captureCurves(const tinygltf::Model&,Budget&);
  [[nodiscard]] bool curvesMatch(const AnimationPlayer*) const noexcept;
  [[nodiscard]] bool authoringMatches() const noexcept;
  [[nodiscard]] bool dependenciesMatch(const AnimationPlayer*) const noexcept;
  ~State() {
    if(pool!=VK_NULL_HANDLE) {
      if(ownsPool) vkDestroyDescriptorPool(device->device(),pool,nullptr);
      else pipelines->destroyDescriptorPool(pool);
    }
    if(buffer.buffer!=VK_NULL_HANDLE) allocation->destroyBuffer(buffer);
  }
};

void SceneManager::PreparedMaterialGeneration::State::captureCurves(const tinygltf::Model& source,Budget& budget) {
  if(!expectedPlayer) return;
  auto curveLimits=limits.animation;curveLimits.authoring=limits.authoring;
  curveLimits.maxOwnedBytes=std::min(curveLimits.maxOwnedBytes,budget.available);
  if(!curveLimits.maxOwnedBytes) invalid("Material curve witness has no remaining preparation budget");
  curves=(player?*player:*expectedPlayer).prepareMaterialEditCurveWitness(source,curveLimits);
  budget.add(curves.ownedBytes());
}
bool SceneManager::PreparedMaterialGeneration::State::curvesMatch(const AnimationPlayer* currentPlayer) const noexcept {
  if(expectedPlayer!=currentPlayer) return false;
  if(!currentPlayer) return !curves && !player;
  return owner && curves && curves.matches(owner->gltfModel_,*currentPlayer) &&
      (!player || curves.matches(owner->gltfModel_,*player));
}
bool SceneManager::PreparedMaterialGeneration::State::authoringMatches() const noexcept {
  if(!owner || (resources && !*resources)) return false;
  if(removal || retirement) return true;
  const auto authored=GltfMaterialSourceView(owner->exportSource_),evaluatedSource=GltfMaterialSourceView(owner->gltfModel_);
  if(appended) return !resourceTransition && GltfMaterialAuthoringPlanMatches(plan,authored,evaluatedSource,limits.authoring);
  if(plan.destinationMaterial<0 || size_t(plan.destinationMaterial)>=owner->exportSource_.materials.size() ||
      size_t(plan.destinationMaterial)>=owner->gltfModel_.materials.size()) return false;
  const auto& authoredBefore=owner->exportSource_.materials[size_t(plan.destinationMaterial)];
  const auto& evaluatedBefore=owner->gltfModel_.materials[size_t(plan.destinationMaterial)];
  if(resourceTransition) return GltfMaterialEditTransitionMatches(plan,authoredBefore,evaluatedBefore,authored,evaluatedSource,
      resources?resources->authoredSourceView():authored,resources?resources->evaluatedSourceView():evaluatedSource,limits.authoring);
  return GltfMaterialEditBeforeMatches(plan,authoredBefore,evaluatedBefore,authored,evaluatedSource,limits.authoring);
}
bool SceneManager::PreparedMaterialGeneration::State::dependenciesMatch(const AnimationPlayer* currentPlayer) const noexcept {
  if(!owner || !curvesMatch(currentPlayer) || beforeDependencies!=owner->materialResourceDependencies_ || !owner->materialResourceDependenciesValid(currentPlayer) ||
      dependencies.size()!=gpu.size() || lifecycleHandles.size()!=gpu.size() || !resourceGeneration ||
      bool(resources)==bool(dependencyChange)) return false;
  if(resources) {
    if(resourceGeneration!=resources->resourceLifecycleGeneration() || !owner->canExchangeMaterialResources(*resources)) return false;
  } else if(resourceGeneration!=dependencyChange->next() ||
      !CanExchangeTextureResourceLifecycle(*dependencyChange,owner->resourceLifecycleGeneration(),owner->textureResourceLifecycleView())) return false;
  const auto mapping=appended?std::span<const uint32_t>(appended->mapping):std::span<const uint32_t>(owner->sourceMaterialToRuntime_);
  for(size_t source=0;source<mapping.size();++source) {
    const auto row=mapping[source];if(row>=dependencies.size()) return false;
    if(dependencies[row] && dependencies[row]->source!=int32_t(source)) return false;
  }
  const auto* restPlayer=player?&*player:currentPlayer;
  for(uint32_t row=0;row<dependencies.size();++row) {
    const bool futureActive=retirement && row==runtime?retirementTarget==MaterialRetirementState::Active:
        (appended && !removal && row==runtime?true:owner->materialIsActive(row));
    const auto& state=dependencies[row];if(!futureActive) {if(state) return false;continue;}
    if(!state) return false;
    if(!(resources?resources->liveOwnerWillBeActive(state->live):dependencyChange->liveOwnerWillBeActive(state->live))) return false;
    MaterialResourceDependencyInput input;input.runtime=row;input.source=state->source;input.material=lifecycleHandles[row];
    input.value=appended?appended->runtime.getMaterial(row):(row==runtime?&converted.value:owner->materialManager_.getMaterial(row));
    if(input.source>=0) {
      const auto source=size_t(input.source);
      if(source>=mapping.size() || mapping[source]!=row) return false;
      if(appended) {
        if(source>=appended->authored.size() || source>=appended->evaluated.size()) return false;
        input.authored=&appended->authored[source];input.evaluated=&appended->evaluated[source];
      } else if(row==runtime) {input.authored=&plan.authoredAfter;input.evaluated=&evaluated;}
      else {
        if(source>=owner->exportSource_.materials.size() || source>=owner->gltfModel_.materials.size()) return false;
        input.authored=&owner->exportSource_.materials[source];input.evaluated=&owner->gltfModel_.materials[source];
      }
      if(restPlayer) {if(source>=restPlayer->library().restMaterials.size()) return false;input.rest=&restPlayer->library().restMaterials[source];}
      else if(state->rest) input.rest=&*state->rest;
    }
    if(appended) {
      if(row<appended->exported.size() && appended->exported[row]) input.exported=&*appended->exported[row];
      if(row<appended->bindings.size() && appended->bindings[row]) input.exportBindings=&*appended->bindings[row];
    } else if(row==runtime) {
      if(exportRecord) input.exported=&*exportRecord;
      if(exportBindings) input.exportBindings=&*exportBindings;
    } else {
      if(row<owner->exportMaterialSources_.size() && owner->exportMaterialSources_[row]) input.exported=&*owner->exportMaterialSources_[row];
      if(row<owner->exportMaterialTextureBindings_.size() && owner->exportMaterialTextureBindings_[row]) input.exportBindings=&*owner->exportMaterialTextureBindings_[row];
    }
    if(row>=provenance.size()) return false;input.provenance=&provenance[row];
    if(!state->matches(input,resourceGeneration,resources?resources->exportTextureIndices():
        std::span<const int32_t>(owner->exportTextureIndices_),true)) return false;
  }
  return true;
}

SceneManager::PreparedMaterialGeneration::PreparedMaterialGeneration() noexcept=default;
SceneManager::PreparedMaterialGeneration::~PreparedMaterialGeneration()=default;
SceneManager::PreparedMaterialGeneration::PreparedMaterialGeneration(PreparedMaterialGeneration&&) noexcept=default;
SceneManager::PreparedMaterialGeneration& SceneManager::PreparedMaterialGeneration::operator=(PreparedMaterialGeneration&&) noexcept=default;
SceneManager::PreparedMaterialGeneration::operator bool() const noexcept { return state_ && !state_->consumed; }
int32_t SceneManager::PreparedMaterialGeneration::sourceMaterial() const noexcept { return state_?state_->plan.destinationMaterial:-1; }
uint32_t SceneManager::PreparedMaterialGeneration::runtimeMaterial() const noexcept { return state_?state_->runtime:UINT32_MAX; }
bool SceneManager::PreparedMaterialGeneration::routingChanged() const noexcept { return state_ && state_->routing; }
bool SceneManager::PreparedMaterialGeneration::hasExtendedPbrMaterials() const noexcept { return state_ && state_->extended; }
uint64_t SceneManager::PreparedMaterialGeneration::materialRevision() const noexcept { return state_?state_->material+1:0; }
uint64_t SceneManager::PreparedMaterialGeneration::generationEpoch() const noexcept { return state_?state_->epoch+1:0; }
std::span<const MaterialRenderProperties> SceneManager::PreparedMaterialGeneration::materialProperties() const noexcept {
  return state_?std::span<const MaterialRenderProperties>(state_->renderProperties):std::span<const MaterialRenderProperties>{};
}
std::span<const container::gpu::GpuMaterial> SceneManager::PreparedMaterialGeneration::gpuMaterials() const noexcept {
  return state_?std::span<const container::gpu::GpuMaterial>(state_->gpu):std::span<const container::gpu::GpuMaterial>{};
}
const container::gpu::AllocatedBuffer& SceneManager::PreparedMaterialGeneration::materialBuffer() const {
  if(!*this) invalid("Material generation is empty or already consumed");return state_->buffer;
}
uint64_t SceneManager::PreparedMaterialGeneration::temporalMaterialRevision(uint32_t material) const noexcept {
  return state_ && material<state_->temporal.size()?state_->temporal[material]:0;
}
bool SceneManager::PreparedMaterialGeneration::rayGeometryCompatible(uint32_t material) const noexcept {
  if(!state_ || material>=state_->gpu.size()) return false;
  const auto& value=state_->gpu[material];
  return value.heightTextureIndex==UINT32_MAX || std::abs(value.heightScale)<=1e-7f;
}
VkDescriptorSet SceneManager::PreparedMaterialGeneration::descriptorSet(uint32_t image) const noexcept {
  return state_ && image<state_->primary.size()?state_->primary[image]:VK_NULL_HANDLE;
}
VkDescriptorSet SceneManager::PreparedMaterialGeneration::auxiliaryDescriptorSet(uint32_t image) const noexcept {
  return state_ && image<state_->auxiliary.size()?state_->auxiliary[image]:VK_NULL_HANDLE;
}
const tinygltf::Material& SceneManager::PreparedMaterialGeneration::evaluatedMaterial() const {
  if(!*this) invalid("Material generation is empty or already consumed");return state_->evaluated;
}
SceneManager::MaterialAppendCommand SceneManager::PreparedMaterialGeneration::appendCommand() const noexcept {
  return state_?state_->command:MaterialAppendCommand{};
}
SceneManager::MaterialRetirementCommand SceneManager::PreparedMaterialGeneration::retirementCommand() const noexcept {
  return state_?state_->retirement:MaterialRetirementCommand{};
}
container::material::TextureImageOwnershipPtr SceneManager::PreparedMaterialGeneration::historyTextureImageOwnership() const noexcept {
  if(!state_) return nullptr;
  return state_->command?state_->command.textureImageOwnership():state_->retirement.textureImageOwnership();
}
TextureResourceReferenceOwnerPtr SceneManager::PreparedMaterialGeneration::historyResourceReferences() const noexcept {
  if(!state_) return nullptr;
  return state_->command?state_->command.resourceReferences():state_->retirement.resourceReferences();
}
std::optional<uint32_t> SceneManager::PreparedMaterialGeneration::retiringMaterial() const noexcept {
  return state_ && state_->retirement && state_->retirementTarget==MaterialRetirementState::Retired?
      std::optional<uint32_t>(state_->runtime):std::nullopt;
}

container::material::TextureImageOwnershipPtr SceneManager::retainMaterialImages(
    std::span<const uint32_t> runtimes,uint64_t maxCpuBytes) const {
  using namespace container::material;
  if(!allocationManager_ || !TextureImageOwnershipMatches(textureImageOwnership_,textureManager_,*allocationManager_))
    invalid("Selected material images require the complete current image ownership generation");
  if(!maxCpuBytes || maxCpuBytes==UINT64_MAX || runtimes.size()>4096)
    invalid("Selected material image retention requires finite capacities");
  const auto generation=resourceLifecycleGeneration();
  if(!generation) invalid("Selected material images require logical source phase authority");
  constexpr size_t slotCount=size_t(GltfMaterialTextureSlot::Count);
  const auto maxSlots=runtimes.size()*(slotCount*4+(config_.experimentalGltfMaterials?experimentalTextureSlots:0));
  Budget budget{maxCpuBytes};budget.add(maxSlots,sizeof(uint32_t));
  const auto selectedBytes=TextureImageOwnershipStorageBytes(maxSlots);budget.add(selectedBytes);
  std::vector<uint32_t> slots;slots.reserve(maxSlots);
  const auto appendRecord=[&](const tinygltf::Material& record,bool authored) {
    GltfMaterialAuthoringLimits limits;limits.maxOutputBytes=std::min(limits.maxOutputBytes,budget.available);
    if(!limits.maxOutputBytes) invalid("Selected material source traversal exceeds its retention budget");
    (void)GltfMaterialRecordStorageBytes(record,limits);
    const auto& roles=authored?archiveTextureResources_:gltfTextureResources_;
    for(const auto& info:GltfMaterialTextureSlotRegistry()) {
      const auto binding=ReadGltfMaterialTextureBinding(record,info.slot);if(binding.index<0) continue;
      if(!generation->active({authored?container::util::ResourceDomainKind::Archive:container::util::ResourceDomainKind::Evaluated,
          container::util::ResourceKind::Texture,uint32_t(binding.index)}))
        invalid("Selected material images reference a retired source texture");
      const auto& indices=(info.srgb || binding.coupledSlot)?roles.color:roles.data;
      if(size_t(binding.index)>=indices.size() || indices[size_t(binding.index)]==UINT32_MAX)
        invalid("A retained selected source binding has no explicit uploaded image role");
      slots.push_back(indices[size_t(binding.index)]);
    }
  };
  for(const auto runtime:runtimes) {
    const auto* value=materialManager_.getMaterial(runtime);
    if(!value || !materialIsActive(runtime)) invalid("Selected material image retention requires active material identities");
    const auto live=container::geometry::gltf::snapshotMaterialTextureBindings(*value).runtimeIndices;
    slots.insert(slots.end(),live.begin(),live.end());
    if(config_.experimentalGltfMaterials) {
      const auto experimental=value->experimental.textureResources();
      slots.insert(slots.end(),experimental.begin(),experimental.end());
    }
    if(runtime<exportMaterialTextureBindings_.size() && exportMaterialTextureBindings_[runtime]) {
      const auto& exported=exportMaterialTextureBindings_[runtime]->runtimeIndices;
      slots.insert(slots.end(),exported.begin(),exported.end());
    }
    const auto source=sourceMaterialForRuntime(runtime);
    if(source>=0) {
      if(size_t(source)>=gltfModel_.materials.size() || size_t(source)>=exportSource_.materials.size())
        invalid("Selected material source identity is unavailable");
      appendRecord(exportSource_.materials[size_t(source)],true);
      appendRecord(gltfModel_.materials[size_t(source)],false);
    }
  }
  for(const auto slot:slots) if(slot!=UINT32_MAX && !generation->active({container::util::ResourceDomainKind::Runtime,
      container::util::ResourceKind::RuntimeTexture,slot})) invalid("Selected material images reference a retired runtime texture");
  return SelectTextureImageOwnership(textureImageOwnership_,slots,*allocationManager_,selectedBytes);
}

container::material::TextureImageOwnershipPtr SceneManager::retainMaterialPlanImages(
    const GltfMaterialAuthoringPlan& plan,const PreparedMaterialResources* resources,uint64_t maxCpuBytes) const {
  using namespace container::material;
  (void)GltfMaterialAuthoringContext(plan);
  if(!allocationManager_ || !TextureImageOwnershipMatches(textureImageOwnership_,textureManager_,*allocationManager_) ||
      (resources && !canPrepareMaterialResources(*resources)))
    invalid("Material history images require coherent current and prospective resource owners");
  if(!maxCpuBytes || maxCpuBytes==UINT64_MAX) invalid("Material history image retention requires a finite budget");
  const auto beforeGeneration=resourceLifecycleGeneration();
  const auto afterGeneration=resources?resources->resourceLifecycleGeneration():beforeGeneration;
  if(!beforeGeneration || !afterGeneration) invalid("Material history images require logical source phase authority");
  constexpr size_t count=size_t(GltfMaterialTextureSlot::Count)+experimentalTextureSlots;
  constexpr size_t beforeCapacity=count*4,afterCapacity=count*2;
  std::array<uint32_t,beforeCapacity> beforeSlots{};std::array<uint32_t,afterCapacity> afterSlots{};
  size_t beforeCount{},afterCount{};
  const auto selectedSlots=size_t(GltfMaterialTextureSlot::Count)+(config_.experimentalGltfMaterials?experimentalTextureSlots:0);
  const auto beforeBytes=TextureImageOwnershipStorageBytes(selectedSlots*4),afterBytes=TextureImageOwnershipStorageBytes(selectedSlots*2);
  const auto unionBytes=TextureImageOwnershipStorageBytes(selectedSlots*6);
  Budget budget{maxCpuBytes};budget.add(beforeBytes);budget.add(afterBytes);budget.add(unionBytes);
  const auto appendRecord=[&](const tinygltf::Material& record,bool authored,bool after,auto& slots,size_t& size) {
    GltfMaterialAuthoringLimits limits;limits.maxOutputBytes=std::min(limits.maxOutputBytes,budget.available);
    if(!limits.maxOutputBytes) invalid("Material history source traversal exceeds its image retention budget");
    (void)GltfMaterialRecordStorageBytes(record,limits);
    const auto append=[&](uint32_t texture,bool color) {
      const auto& generation=after?afterGeneration:beforeGeneration;
      if(!generation->active({authored?container::util::ResourceDomainKind::Archive:container::util::ResourceDomainKind::Evaluated,
          container::util::ResourceKind::Texture,texture}))
        invalid("Material history images reference a retired source texture");
      uint32_t runtime=UINT32_MAX;
      if(after && resources && authored) runtime=resources->reference(texture,color).runtimeResource();
      else {
        const auto context=after && resources?resources->conversionContext():GltfMaterialConversionContext{
            authored?exportSource_.textures.size():gltfModel_.textures.size(),{},
            authored?std::span<const uint32_t>(archiveTextureResources_.color):std::span<const uint32_t>(gltfTextureResources_.color),
            authored?std::span<const uint32_t>(archiveTextureResources_.data):std::span<const uint32_t>(gltfTextureResources_.data),textureManager_.textureCount()};
        const auto indices=color?context.colorResources:context.dataResources;
        if(texture>=indices.size()) invalid("Material history source texture identity is unavailable");
        runtime=indices[texture];
      }
      if(runtime==UINT32_MAX || size>=slots.size()) invalid("Material history image roles are unavailable or exceed their selected capacity");
      slots[size++]=runtime;
    };
    for(const auto& info:GltfMaterialTextureSlotRegistry()) {
      const auto binding=ReadGltfMaterialTextureBinding(record,info.slot);
      if(binding.index>=0) append(uint32_t(binding.index),info.srgb || binding.coupledSlot.has_value());
    }
    if(config_.experimentalGltfMaterials) {
      const auto view=after && resources?(authored?resources->authoredSourceView():resources->evaluatedSourceView()):
          GltfMaterialSourceView(authored?exportSource_:gltfModel_);
      visitExperimentalMaterialTextures(record,view.extensionsUsed,view.textureCount,append);
    }
  };
  if(plan.sourceMaterial) {
    const auto source=*plan.sourceMaterial;
    if(!authoredMaterialIsActive(source)) invalid("Material history image source is retired");
    const auto runtime=resolveLoadedMaterialIndex(source);const auto* value=materialManager_.getMaterial(runtime);
    if(!value) invalid("Material history runtime source is unavailable");
    const auto actual=container::geometry::gltf::snapshotMaterialTextureBindings(*value).runtimeIndices;
    std::copy(actual.begin(),actual.end(),beforeSlots.begin()+beforeCount);beforeCount+=actual.size();
    if(config_.experimentalGltfMaterials) {
      const auto experimental=value->experimental.textureResources();
      std::copy(experimental.begin(),experimental.end(),beforeSlots.begin()+beforeCount);beforeCount+=experimental.size();
    }
    if(runtime<exportMaterialTextureBindings_.size() && exportMaterialTextureBindings_[runtime]) {
      const auto& exported=exportMaterialTextureBindings_[runtime]->runtimeIndices;
      std::copy(exported.begin(),exported.end(),beforeSlots.begin()+beforeCount);beforeCount+=exported.size();
    }
  }
  appendRecord(plan.authoredBefore,true,false,beforeSlots,beforeCount);
  appendRecord(plan.evaluatedBefore,false,false,beforeSlots,beforeCount);
  appendRecord(plan.authoredAfter,true,true,afterSlots,afterCount);
  appendRecord(plan.evaluatedAfter,false,true,afterSlots,afterCount);
  for(const auto slot:std::span(beforeSlots.data(),beforeCount)) if(slot!=UINT32_MAX && !beforeGeneration->active({
      container::util::ResourceDomainKind::Runtime,container::util::ResourceKind::RuntimeTexture,slot}))
    invalid("Material history images reference a retired before runtime texture");
  for(const auto slot:std::span(afterSlots.data(),afterCount)) if(slot!=UINT32_MAX && !afterGeneration->active({
      container::util::ResourceDomainKind::Runtime,container::util::ResourceKind::RuntimeTexture,slot}))
    invalid("Material history images reference a retired prospective runtime texture");
  const auto prospective=resources?resources->textureImageOwnership():textureImageOwnership_;
  const std::array selected{
      SelectTextureImageOwnership(textureImageOwnership_,std::span(beforeSlots.data(),beforeCount),*allocationManager_,beforeBytes),
      SelectTextureImageOwnership(prospective,std::span(afterSlots.data(),afterCount),*allocationManager_,afterBytes)};
  return CombineTextureImageOwnership(selected,*allocationManager_,unionBytes);
}

TextureResourceReferenceOwnerPtr SceneManager::retainMaterialResourceReferences(
    std::span<const uint32_t> runtimes,uint64_t maxCpuBytes) const try {
  using namespace container::util;
  if(!maxCpuBytes || maxCpuBytes==UINT64_MAX || runtimes.size()>4096)
    invalid("Selected material references require finite capacities");
  refreshTextureResourceLifecycle(ResourceRetirementPolicy::InvalidatePreparations,maxCpuBytes);
  const auto generation=resourceLifecycleGeneration();
  if(!generation || !generation->matches(textureResourceLifecycleView()) ||
      !materialLifecycle_ || materialLifecycleHandles_.size()!=materialManager_.materialCount())
    invalid("Selected material references require the exact current logical resource generation");
  Budget budget{maxCpuBytes};budget.add(4096);budget.add(runtimes.size(),sizeof(ResourceHandle));
  MaterialResourceSelections selected{budget};std::vector<ResourceHandle> materials;materials.reserve(runtimes.size());
  const auto record=[&](const tinygltf::Material& value,bool archive) {
    const auto domain=archive?ResourceDomainKind::Archive:ResourceDomainKind::Evaluated;
    selected.record(value,domain,generation);
    const auto& roles=archive?archiveTextureResources_:gltfTextureResources_;
    for(const auto& info:GltfMaterialTextureSlotRegistry()) {
      const auto binding=ReadGltfMaterialTextureBinding(value,info.slot);if(binding.index<0) continue;
      const auto& indices=(info.srgb || binding.coupledSlot)?roles.color:roles.data;
      if(size_t(binding.index)>=indices.size() || indices[size_t(binding.index)]==UINT32_MAX)
        invalid("A selected source material has no exact runtime resource role");
      selected.add(ResourceDomainKind::Runtime,ResourceKind::RuntimeTexture,indices[size_t(binding.index)]);
    }
  };
  for(const auto runtime:runtimes) {
    const auto* value=materialManager_.getMaterial(runtime);
    if(!value || !materialIsActive(runtime)) invalid("Selected material references require active material incarnations");
    materials.push_back(materialLifecycleHandles_[runtime]);
    selected.runtime(snapshotMaterialTextureBindings(*value).runtimeIndices);
    selected.runtime(value->experimental.textureResources());
    if(runtime<exportMaterialTextureBindings_.size() && exportMaterialTextureBindings_[runtime])
      selected.runtime(exportMaterialTextureBindings_[runtime]->runtimeIndices);
    const auto source=sourceMaterialForRuntime(runtime);
    if(source>=0) {record(exportSource_.materials[size_t(source)],true);record(gltfModel_.materials[size_t(source)],false);}
    if(runtime<exportMaterialSources_.size() && exportMaterialSources_[runtime]) record(*exportMaterialSources_[runtime],true);
  }
  selected.requireActive(generation);
  auto owner=RetainTextureResourceReferences(generation,selected.rows,ResourcePinClass::History,budget.available,selected.opaque);
  budget.add(owner->ownedBytes());
  if(!materials.empty()) {
    auto pin=materialLifecycle_->pin(materials,ResourcePinClass::History,"Selected material binding and endpoint history");
    owner=AddTextureResourceReferencePin(owner,std::move(pin),budget.available);
  }
  return owner;
} catch(const std::length_error& error) {
  throw std::invalid_argument(error.what());
}

TextureResourceReferenceOwnerPtr SceneManager::retainMaterialPlanResourceReferences(
    const GltfMaterialAuthoringPlan& plan,const PreparedMaterialResources* resources,uint64_t maxCpuBytes) const try {
  using namespace container::util;
  (void)GltfMaterialAuthoringContext(plan);
  if(!maxCpuBytes || maxCpuBytes==UINT64_MAX) invalid("Material history references require a finite budget");
  refreshTextureResourceLifecycle(ResourceRetirementPolicy::InvalidatePreparations,maxCpuBytes);
  const auto before=resourceLifecycleGeneration();
  const auto after=resources?resources->resourceLifecycleGeneration():before;
  if(!before || !after || !before->matches(textureResourceLifecycleView()) ||
      (resources && !canPrepareMaterialResources(*resources)))
    invalid("Material history references require coherent current and prospective logical resource generations");
  Budget budget{maxCpuBytes};budget.add(4096);
  MaterialResourceSelections oldRows{budget},newRows{budget};
  const auto record=[&](const tinygltf::Material& value,bool archive,bool prospective,MaterialResourceSelections& selected) {
    selected.record(value,archive?ResourceDomainKind::Archive:ResourceDomainKind::Evaluated,prospective?after:before);
    const auto select=[&](uint32_t texture,bool color) {
      uint32_t runtime=UINT32_MAX;
      if(prospective && resources && archive) runtime=resources->reference(texture,color).runtimeResource();
      else {
        const auto context=prospective && resources?resources->conversionContext():container::material::GltfMaterialConversionContext{
            archive?exportSource_.textures.size():gltfModel_.textures.size(),{},
            archive?std::span<const uint32_t>(archiveTextureResources_.color):std::span<const uint32_t>(gltfTextureResources_.color),
            archive?std::span<const uint32_t>(archiveTextureResources_.data):std::span<const uint32_t>(gltfTextureResources_.data),textureManager_.textureCount()};
        const auto indices=color?context.colorResources:context.dataResources;
        if(texture>=indices.size()) invalid("Selected material history has an unavailable source role");
        runtime=indices[texture];
      }
      if(runtime==UINT32_MAX) invalid("Selected material history has an unuploaded runtime resource role");
      selected.add(ResourceDomainKind::Runtime,ResourceKind::RuntimeTexture,runtime);
    };
    for(const auto& info:GltfMaterialTextureSlotRegistry()) {
      const auto binding=ReadGltfMaterialTextureBinding(value,info.slot);
      if(binding.index>=0) select(uint32_t(binding.index),info.srgb || binding.coupledSlot.has_value());
    }
    if(config_.experimentalGltfMaterials) {
      const auto view=prospective && resources?(archive?resources->authoredSourceView():resources->evaluatedSourceView()):
          GltfMaterialSourceView(archive?exportSource_:gltfModel_);
      visitExperimentalMaterialTextures(value,view.extensionsUsed,view.textureCount,select);
    }
  };
  std::optional<ResourceHandle> material;
  if(plan.sourceMaterial) {
    const auto source=*plan.sourceMaterial;
    if(!authoredMaterialIsActive(source)) invalid("Selected material history has a retired source incarnation");
    const auto runtime=resolveLoadedMaterialIndex(source);const auto* value=materialManager_.getMaterial(runtime);
    if(!value || !materialLifecycle_ || runtime>=materialLifecycleHandles_.size()) invalid("Selected material history has no logical material owner");
    material=materialLifecycleHandles_[runtime];oldRows.runtime(snapshotMaterialTextureBindings(*value).runtimeIndices);
    oldRows.runtime(value->experimental.textureResources());
    if(runtime<exportMaterialTextureBindings_.size() && exportMaterialTextureBindings_[runtime])
      oldRows.runtime(exportMaterialTextureBindings_[runtime]->runtimeIndices);
    if(runtime<exportMaterialSources_.size() && exportMaterialSources_[runtime]) record(*exportMaterialSources_[runtime],true,false,oldRows);
  }
  record(plan.authoredBefore,true,false,oldRows);record(plan.evaluatedBefore,false,false,oldRows);
  record(plan.authoredAfter,true,true,newRows);record(plan.evaluatedAfter,false,true,newRows);
  oldRows.requireActive(before);newRows.requireActive(after);
  auto oldOwner=RetainTextureResourceReferences(before,oldRows.rows,ResourcePinClass::History,budget.available,oldRows.opaque);
  budget.add(oldOwner->ownedBytes());
  auto newOwner=resources?resources->retainResourceReferences(newRows.rows,ResourcePinClass::History,budget.available,newRows.opaque):
      RetainTextureResourceReferences(after,newRows.rows,ResourcePinClass::History,budget.available,newRows.opaque);
  budget.add(newOwner->ownedBytes());
  const std::array owners{oldOwner,newOwner};auto owner=CombineTextureResourceReferences(owners,budget.available);
  budget.add(owner->ownedBytes());
  if(material) {
    auto pin=materialLifecycle_->pin(std::span(&*material,1),ResourcePinClass::History,"Selected material authoring endpoint history");
    owner=AddTextureResourceReferencePin(owner,std::move(pin),budget.available);
  }
  return owner;
} catch(const std::length_error& error) {
  throw std::invalid_argument(error.what());
}

TextureResourceReferenceOwnerPtr SceneManager::retainSceneResourceReferences(
    const PreparedMaterialResources* resources,uint64_t maxCpuBytes) const try {
  using namespace container::util;
  if(!maxCpuBytes || maxCpuBytes==UINT64_MAX)
    invalid("Scene resource preparation requires an exact logical generation and finite budget");
  refreshTextureResourceLifecycle(ResourceRetirementPolicy::InvalidatePreparations,maxCpuBytes);
  const auto generation=resources?resources->resourceLifecycleGeneration():resourceLifecycleGeneration();
  if(!generation || (resources && !canPrepareMaterialResources(*resources)))
    invalid("Scene resource preparation requires an exact logical generation and finite budget");
  if(!resources) {
    if(!generation->matches(textureResourceLifecycleView()))
      invalid("Scene resource preparation belongs to a stale logical generation");
    return RetainAllTextureResourceReferences(generation,ResourcePinClass::Preparation,maxCpuBytes);
  }
  return resources->retainAllResourceReferences(ResourcePinClass::Preparation,maxCpuBytes);
} catch(const std::length_error& error) {
  throw std::invalid_argument(error.what());
}

struct SceneManager::PreparedSceneBindings::State {
  const SceneManager* owner{};
  std::shared_ptr<const uint8_t> ownerIdentity;
  std::shared_ptr<container::gpu::VulkanDevice> device;
  container::gpu::PipelineManager* pipelines{};
  SceneBindingsLimits limits;
  uint64_t epoch{},geometry{},material{},authored{},catalog{},resourceRevision{},descriptorCpuAvailable{};
  size_t materialCount{},metadataCount{};
  VkDescriptorPool expectedPool{},pool{};
  VkDescriptorSetLayout layout{};
  VkBuffer materialBuffer{},metadataBuffer{},clipBuffer{};
  VmaAllocation materialAllocation{},metadataAllocation{},clipAllocation{};
  uint32_t textureCapacity{},samplerCapacity{};
  std::shared_ptr<const TextureSamplingFallback> samplingFallback;
  container::material::TextureImageOwnershipPtr images;
  TextureResourceLifecycleGenerationPtr resourceGeneration;
  TextureResourceReferenceOwnerPtr readerReferences,references;
  container::material::TextureSamplingPlan sampling;
  // Resolved descriptor-order handles also witness the live sampler slot map.
  std::vector<VkSampler> samplers;
  std::shared_ptr<const PreparedSourceResourceRetirement> prospectiveResources;
  std::shared_ptr<const uint8_t> descriptorFrameWitness;
  std::vector<VkDescriptorSet> primary,auxiliary;
  bool expectedOwnsPool{},ownsPool{true},ready{},consumed{};
  ~State() {
    if(pool!=VK_NULL_HANDLE) {
      if(ownsPool) vkDestroyDescriptorPool(device->device(),pool,nullptr);
      else pipelines->destroyDescriptorPool(pool);
    }
  }
};

SceneManager::PreparedSceneBindings::PreparedSceneBindings() noexcept=default;
SceneManager::PreparedSceneBindings::~PreparedSceneBindings()=default;
SceneManager::PreparedSceneBindings::PreparedSceneBindings(PreparedSceneBindings&&) noexcept=default;
SceneManager::PreparedSceneBindings& SceneManager::PreparedSceneBindings::operator=(PreparedSceneBindings&&) noexcept=default;
SceneManager::PreparedSceneBindings::operator bool() const noexcept {return state_ && !state_->consumed;}
uint64_t SceneManager::PreparedSceneBindings::generationEpoch() const noexcept {return state_?state_->epoch+1:0;}
VkDescriptorSet SceneManager::PreparedSceneBindings::descriptorSet(uint32_t image) const noexcept {
  return state_ && image<state_->primary.size()?state_->primary[image]:VK_NULL_HANDLE;
}
VkDescriptorSet SceneManager::PreparedSceneBindings::auxiliaryDescriptorSet(uint32_t image) const noexcept {
  return state_ && image<state_->auxiliary.size()?state_->auxiliary[image]:VK_NULL_HANDLE;
}

SceneManager::PreparedSceneBindings SceneManager::prepareSceneBindings(const SceneBindingsLimits& limits) const try {
  if(!deviceWrapper_ || !pipelineManager_ || descriptorPool_==VK_NULL_HANDLE || descriptorSetLayout_==VK_NULL_HANDLE ||
      materialBuffer_.buffer==VK_NULL_HANDLE || textureMetadataBuffer_.buffer==VK_NULL_HANDLE ||
      sceneClipStateBuffer_.buffer==VK_NULL_HANDLE || generationEpoch_==UINT64_MAX)
    invalid("Live scene descriptor resources or publication epoch are unavailable");
  if(!limits.maxPreparedCpuBytes || limits.maxPreparedCpuBytes==UINT64_MAX ||
      !limits.maxDescriptorFrames || limits.maxDescriptorFrames>1024)
    invalid("Scene descriptor staging requires finite capacities");
  refreshTextureResourceLifecycle(container::util::ResourceRetirementPolicy::InvalidatePreparations,limits.maxPreparedCpuBytes);
  const auto textures=textureManager_.textureCount();
  if(!textureDescriptorCapacity_ || !materialSamplerDescriptorCapacity_ || textures>textureDescriptorCapacity_ ||
      materialSamplerSlots_.keys.size()>materialSamplerDescriptorCapacity_ || gpuMaterials_.empty() ||
      gpuMaterials_.size()!=materialManager_.materialCount() || gpuMaterials_.size()>UINT32_MAX)
    invalid("Live scene descriptor resource tables disagree");
  Budget budget{limits.maxPreparedCpuBytes};budget.add(sizeof(PreparedSceneBindings::State));
  const auto samplingBytes=container::material::TextureSamplingPlanStorageBytes(textures);
  budget.add(samplingBytes);budget.add(materialSamplerDescriptorCapacity_,sizeof(VkSampler));
  if(!container::material::TextureImageOwnershipMatches(textureImageOwnership_,textureManager_,*allocationManager_))
    invalid("Scene descriptor preparation requires complete image pin coverage");
  budget.add(textureImageOwnership_->ownedBytes());
  if(!textureSamplingStateValid())
    invalid("Scene descriptors require coherent owned texture sampling state");
  if(!materialResourceDependenciesValid()) invalid("Scene descriptors require current material live dependency controls");
  // Capture phases and allocator-issued identities, not only resolved views:
  // active aliases and retired rows can deliberately resolve to the same view.
  auto sampling=container::material::MakeTextureSamplingPlan(textureManager_,textureSamplingFallback_->resource,
      *allocationManager_,textureDescriptorCapacity_,materialSamplerDescriptorCapacity_,samplingBytes);
  if(baseColorSampler_==VK_NULL_HANDLE) invalid("Scene descriptors require the default sampler");
  for(const auto key:materialSamplerSlots_.keys)
    if(key.value>=materialSamplers_.size() || materialSamplers_[key.value]==VK_NULL_HANDLE)
      invalid("Scene descriptors reference an unavailable material sampler");
  const auto resourceGeneration=resourceLifecycleGeneration();
  if(!resourceGeneration || !resourceGeneration->matches(textureResourceLifecycleView()))
    invalid("Scene descriptors require the current logical resource generation");
  auto readerReferences=RetainAllTextureResourceReferences(resourceGeneration,
      container::util::ResourcePinClass::GpuReader,budget.available);budget.add(readerReferences->ownedBytes());
  auto references=retainSceneResourceReferences(nullptr,budget.available);budget.add(references->ownedBytes());
  PreparedSceneBindings result;result.state_=std::make_unique<PreparedSceneBindings::State>();auto& s=*result.state_;
  s.owner=this;s.ownerIdentity=sceneRootOwnerIdentity_;s.device=deviceWrapper_;s.pipelines=pipelineManager_;s.limits=limits;
  s.epoch=generationEpoch_;s.geometry=geometryRevision_;s.material=materialRevision_;s.authored=authoredMaterialRevision_;
  s.catalog=sceneRootCatalogIdentity_;s.resourceRevision=materialResourceRevision_;s.descriptorCpuAvailable=budget.available;
  s.expectedPool=descriptorPool_;s.expectedOwnsPool=ownsDescriptorPool_;s.layout=descriptorSetLayout_;
  s.materialBuffer=materialBuffer_.buffer;s.materialAllocation=materialBuffer_.allocation;s.materialCount=gpuMaterials_.size();
  s.metadataBuffer=textureMetadataBuffer_.buffer;s.metadataAllocation=textureMetadataBuffer_.allocation;s.metadataCount=textureMetadata_.size();
  s.clipBuffer=sceneClipStateBuffer_.buffer;s.clipAllocation=sceneClipStateBuffer_.allocation;
  s.textureCapacity=textureDescriptorCapacity_;s.samplerCapacity=materialSamplerDescriptorCapacity_;
  s.samplingFallback=textureSamplingFallback_;s.sampling=std::move(sampling);s.images=textureImageOwnership_;
  s.resourceGeneration=resourceGeneration;s.readerReferences=std::move(readerReferences);s.references=std::move(references);
  s.samplers.reserve(s.samplerCapacity);
  for(uint32_t i=0;i<s.samplerCapacity;++i)
    s.samplers.push_back(i<materialSamplerSlots_.keys.size()?materialSamplers_[materialSamplerSlots_.keys[i].value]:baseColorSampler_);
  return result;
} catch(const std::length_error& error) {
  throw std::invalid_argument(error.what());
}

SceneManager::PreparedSceneBindings SceneManager::prepareSceneBindings(const PreparedSourceResourceRetirement& resources,
    const SceneBindingsLimits& limits,const AnimationPlayer* currentPlayer) const {
  if(!resources.publicationMatches(*this,false,currentPlayer)) invalid("Scene descriptor resource candidate is stale");
  auto result=prepareSceneBindings(limits);auto& s=*result.state_;Budget budget{s.descriptorCpuAvailable};
  budget.add(sizeof(PreparedSourceResourceRetirement)+128);
  budget.add(container::material::TextureSamplingPlanStorageBytes(resources.textureResourceCount()));
  const auto images=resources.textureImageOwnership();if(!images) invalid("Scene descriptor resource candidate has no complete image owner");
  budget.add(images->ownedBytes());
  auto wrapper=std::make_shared<PreparedSourceResourceRetirement>();wrapper->state_=resources.state_;
  s.prospectiveResources=std::move(wrapper);s.images=images;s.resourceGeneration=resources.resourceLifecycleGeneration();
  s.references=resources.retainAllResourceReferences(container::util::ResourcePinClass::Preparation,budget.available);
  budget.add(s.references->ownedBytes());s.sampling=resources.samplingPlan();
  const auto& metadata=resources.metadataBuffer();s.metadataBuffer=metadata.buffer;s.metadataAllocation=metadata.allocation;
  s.metadataCount=s.sampling.metadata.size();
  if(resources.runtimeChanged()) {
    const auto samplers=resources.descriptorSamplers();budget.add(samplers.size(),sizeof(VkSampler));
    s.samplers.assign(samplers.begin(),samplers.end());
  }
  s.descriptorCpuAvailable=budget.available;
  if(!sceneBindingsMatch(result,currentPlayer)) invalid("Scene descriptor resource candidate changed during capture");
  return result;
}

bool SceneManager::sceneBindingsMatch(const PreparedSceneBindings& token,const AnimationPlayer* currentPlayer) const noexcept {
  if(!token) return false;const auto& s=*token.state_;
  if(s.prospectiveResources) {
    const bool before=s.prospectiveResources->publicationMatches(*this,false,currentPlayer);
    const bool after=!before && s.prospectiveResources->publicationMatches(*this,true,currentPlayer);
    if((!before && !after) || s.owner!=this || s.ownerIdentity!=sceneRootOwnerIdentity_ || s.epoch!=generationEpoch_ ||
        s.geometry!=geometryRevision_ || s.material!=materialRevision_ || s.authored!=authoredMaterialRevision_ ||
        s.catalog!=sceneRootCatalogIdentity_ || (before?s.resourceRevision!=materialResourceRevision_:
            (s.resourceRevision==UINT64_MAX || s.resourceRevision+1!=materialResourceRevision_)) ||
        s.samplingFallback!=textureSamplingFallback_ || !s.readerReferences || !s.readerReferences->valid() ||
        !s.references || !s.references->valid() || s.resourceGeneration!=s.prospectiveResources->resourceLifecycleGeneration() ||
        s.images!=s.prospectiveResources->textureImageOwnership() || s.expectedPool!=descriptorPool_ ||
        s.expectedOwnsPool!=ownsDescriptorPool_ || s.layout!=descriptorSetLayout_ || s.materialBuffer!=materialBuffer_.buffer ||
        s.materialAllocation!=materialBuffer_.allocation || s.materialCount!=gpuMaterials_.size() ||
        s.materialCount!=materialManager_.materialCount() || s.clipBuffer!=sceneClipStateBuffer_.buffer ||
        s.clipAllocation!=sceneClipStateBuffer_.allocation || s.textureCapacity!=textureDescriptorCapacity_ ||
        s.samplerCapacity!=materialSamplerDescriptorCapacity_) return false;
    if(before) {
      const auto& resources=*s.prospectiveResources;
      const auto& metadata=resources.metadataBuffer();
      if(s.metadataBuffer!=metadata.buffer || s.metadataAllocation!=metadata.allocation ||
          !container::material::TextureImageOwnershipMatches(s.images,resources.textureResources(),*allocationManager_) ||
          !container::material::TextureSamplingPlanMatches(s.sampling,resources.textureResources(),textureSamplingFallback_->resource,*allocationManager_)) return false;
      if(resources.runtimeChanged()) {
        const auto samplers=resources.descriptorSamplers();
        if(s.samplers.size()!=samplers.size() || !std::equal(s.samplers.begin(),s.samplers.end(),samplers.begin())) return false;
      }
    } else {
      if(s.resourceGeneration!=textureResourceLifecycle_ || s.images!=textureImageOwnership_ ||
          s.metadataBuffer!=textureMetadataBuffer_.buffer || s.metadataAllocation!=textureMetadataBuffer_.allocation ||
          s.metadataCount!=textureMetadata_.size() || !textureSamplingStateValid() ||
          !container::material::TextureImageOwnershipMatches(s.images,textureManager_,*allocationManager_) ||
          !container::material::TextureSamplingPlanMatches(s.sampling,textureManager_,textureSamplingFallback_->resource,*allocationManager_)) return false;
    }
    if(after || !s.prospectiveResources->runtimeChanged()) for(size_t slot=0;slot<s.samplers.size();++slot) {
      VkSampler sampler=baseColorSampler_;
      if(slot<materialSamplerSlots_.keys.size()) {
        const auto key=materialSamplerSlots_.keys[slot];if(key.value>=materialSamplers_.size()) return false;sampler=materialSamplers_[key.value];
      }
      if(s.samplers[slot]!=sampler) return false;
    }
    return s.metadataCount==s.sampling.metadata.size();
  }
  if(s.owner!=this || s.ownerIdentity!=sceneRootOwnerIdentity_ || s.epoch!=generationEpoch_ || s.geometry!=geometryRevision_ ||
      s.material!=materialRevision_ || s.authored!=authoredMaterialRevision_ || s.catalog!=sceneRootCatalogIdentity_ ||
      s.resourceRevision!=materialResourceRevision_ || s.samplingFallback!=textureSamplingFallback_ ||
      s.resourceGeneration!=resourceLifecycleGeneration() || !s.resourceGeneration ||
      !s.resourceGeneration->matches(textureResourceLifecycleView()) ||
      !s.readerReferences || !s.readerReferences->valid() || !s.references || !s.references->valid() || !materialResourceDependenciesValid() ||
      s.images!=textureImageOwnership_ || !container::material::TextureImageOwnershipMatches(s.images,textureManager_,*allocationManager_) ||
      s.expectedPool!=descriptorPool_ || s.expectedOwnsPool!=ownsDescriptorPool_ || s.layout!=descriptorSetLayout_ ||
      s.materialBuffer!=materialBuffer_.buffer || s.materialAllocation!=materialBuffer_.allocation ||
      s.materialCount!=gpuMaterials_.size() || s.materialCount!=materialManager_.materialCount() ||
      s.metadataBuffer!=textureMetadataBuffer_.buffer || s.metadataAllocation!=textureMetadataBuffer_.allocation ||
      s.metadataCount!=textureMetadata_.size() || s.clipBuffer!=sceneClipStateBuffer_.buffer || s.clipAllocation!=sceneClipStateBuffer_.allocation ||
      s.textureCapacity!=textureDescriptorCapacity_ || s.samplerCapacity!=materialSamplerDescriptorCapacity_ ||
      materialSamplerSlots_.keys.size()>s.samplers.size() || !textureSamplingStateValid() ||
      !container::material::TextureSamplingPlanMatches(s.sampling,textureManager_,textureSamplingFallback_->resource,*allocationManager_)) return false;
  for(size_t i=0;i<s.samplers.size();++i) {
    VkSampler sampler=baseColorSampler_;
    if(i<materialSamplerSlots_.keys.size()) {
      const auto key=materialSamplerSlots_.keys[i];if(key.value>=materialSamplers_.size()) return false;
      sampler=materialSamplers_[key.value];
    }
    if(sampler!=s.samplers[i]) return false;
  }
  return true;
}

void SceneManager::PreparedSceneBindings::prepareDescriptorFrames(
    std::span<const container::gpu::AllocatedBuffer> cameras,std::span<const container::gpu::AllocatedBuffer> objects,
    std::span<const container::gpu::AllocatedBuffer> auxiliaryObjects,const AnimationPlayer* currentPlayer) {
  if(!*this) invalid("Scene descriptor generation is empty or consumed");auto& s=*state_;const auto& owner=*s.owner;
  if(s.pool!=VK_NULL_HANDLE) invalid("Scene descriptor generation has already been prepared");
  if(cameras.empty() || cameras.size()>s.limits.maxDescriptorFrames || objects.empty() ||
      (objects.size()!=1 && objects.size()!=cameras.size()) ||
      (!auxiliaryObjects.empty() && auxiliaryObjects.size()!=1 && auxiliaryObjects.size()!=cameras.size()))
    invalid("Scene descriptor frame tables have incompatible shapes");
  if(!owner.sceneBindingsMatch(*this,currentPlayer)) invalid("Scene descriptor generation no longer belongs to the live asset");
  if(auxiliaryObjects.empty()) auxiliaryObjects=objects;
  for(const auto& camera:cameras)
    if(camera.buffer==VK_NULL_HANDLE || camera.allocation_info.size<sizeof(container::gpu::CameraData))
      invalid("Scene descriptor frame references an unavailable camera buffer");
  for(const auto table:{objects,auxiliaryObjects}) for(const auto& object:table)
    if(object.buffer==VK_NULL_HANDLE || object.allocation_info.size<sizeof(container::gpu::ObjectData))
      invalid("Scene descriptor frame references an unavailable object buffer");
  const auto frames=uint32_t(cameras.size()),sets=frames*2;
  Budget budget{s.descriptorCpuAvailable};budget.add(sets,2*sizeof(VkDescriptorSet)+sizeof(VkDescriptorSetLayout));
  if(s.prospectiveResources) budget.add(128);
  budget.add(container::material::TextureSamplingPlanStorageBytes(owner.textureManager_.textureCount()));
  budget.add(s.samplers.size(),sizeof(VkDescriptorImageInfo));budget.add(16,sizeof(VkWriteDescriptorSet));
  for(const auto count:{s.textureCapacity,s.samplerCapacity})
    if(count>UINT32_MAX/sets) invalid("Scene descriptor pool counts overflow");
  const std::array sizes{
      VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,sets*2},
      VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,sets*3},
      VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,sets*s.textureCapacity},
      VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_SAMPLER,sets*s.samplerCapacity}};
  VkDescriptorPoolCreateInfo create{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  create.maxSets=sets;create.poolSizeCount=uint32_t(sizes.size());create.pPoolSizes=sizes.data();
  if(vkCreateDescriptorPool(s.device->device(),&create,nullptr,&s.pool)!=VK_SUCCESS)
    throw std::runtime_error("Failed to create detached scene descriptor pool");
  std::vector<VkDescriptorSetLayout> layouts(sets,s.layout);std::vector<VkDescriptorSet> allocated(sets);
  VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  allocate.descriptorPool=s.pool;allocate.descriptorSetCount=sets;allocate.pSetLayouts=layouts.data();
  if(vkAllocateDescriptorSets(s.device->device(),&allocate,allocated.data())!=VK_SUCCESS)
    throw std::runtime_error("Failed to allocate detached scene descriptor sets");
  s.primary.assign(allocated.begin(),allocated.begin()+frames);s.auxiliary.assign(allocated.begin()+frames,allocated.end());
  for(uint32_t i=0;i<frames;++i) {
    if(s.prospectiveResources) {
      s.prospectiveResources->writeDescriptorSetContents(s.primary[i],cameras[i],objects[std::min<size_t>(i,objects.size()-1)],owner.materialBuffer_,s.materialCount,currentPlayer);
      s.prospectiveResources->writeDescriptorSetContents(s.auxiliary[i],cameras[i],auxiliaryObjects[std::min<size_t>(i,auxiliaryObjects.size()-1)],owner.materialBuffer_,s.materialCount,currentPlayer);
    } else {
      owner.writeMaterialDescriptorSetContents(s.primary[i],cameras[i],objects[std::min<size_t>(i,objects.size()-1)],owner.materialBuffer_,s.materialCount);
      owner.writeMaterialDescriptorSetContents(s.auxiliary[i],cameras[i],auxiliaryObjects[std::min<size_t>(i,auxiliaryObjects.size()-1)],owner.materialBuffer_,s.materialCount);
    }
  }
  if(s.prospectiveResources) {
    s.descriptorFrameWitness=std::make_shared<const uint8_t>(0);
    if(!s.prospectiveResources->markDescriptorFramesReady(s.descriptorFrameWitness))
      invalid("Resource descriptor publication exceeds its prepared binding capacity");
  }
  s.ready=true;
}

bool SceneManager::canExchangeSceneBindings(const PreparedSceneBindings& token,const AnimationPlayer* currentPlayer) const noexcept {
  return sceneBindingsMatch(token,currentPlayer) && token.state_->ready;
}
void SceneManager::exchangeSceneBindings(PreparedSceneBindings& token,const AnimationPlayer* currentPlayer) noexcept {
  if(!canExchangeSceneBindings(token,currentPlayer)) return;auto& s=*token.state_;using std::swap;
  swap(descriptorPool_,s.pool);swap(ownsDescriptorPool_,s.ownsPool);
  descriptorSets_.swap(s.primary);auxiliaryDescriptorSets_.swap(s.auxiliary);
  ++generationEpoch_;s.consumed=true;
}

std::optional<uint32_t> SceneManager::materialNormalTexCoord(uint32_t runtime) const {
  const auto* material=materialManager_.getMaterial(runtime);
  if(!material) invalid("Normal texture coordinate query requires an existing runtime material");
  if(material->normalTextureIndex==UINT32_MAX) return std::nullopt;
  return material->normalTextureTransform.texCoord;
}

uint64_t SceneManager::materialLifecyclePreparationBytes(size_t count) {
  if(count>1'000'000 || count>(UINT64_MAX-16*1024)/512) invalid("Material lifecycle initialization exceeds its finite row budget");
  return uint64_t(count)*512+16*1024;
}
void SceneManager::resetMaterialLifecycle(std::span<const uint8_t> retired) {
  using namespace container::util;
  const auto count=materialManager_.materialCount();
  if(materialLifecyclePreparationBytes(count)>512ull*1024*1024 || (!retired.empty() && retired.size()!=count))
    invalid("Material lifecycle initialization has an invalid phase map or preparation budget");
  for(const auto phase:retired) if(phase>1) invalid("Material lifecycle phase map is invalid");
  auto catalog=std::make_shared<ResourceLifecycleCatalog>();const auto domain=catalog->createDomain(ResourceDomainKind::Runtime);
  std::vector<ResourceRegistrationRequest> requests;requests.reserve(count);
  for(uint32_t row=0;row<count;++row) requests.push_back({domain,ResourceKind::Material,row});
  std::vector<ResourceHandle> handles;handles.reserve(count);
  if(count) {
    auto registration=catalog->prepareIssue(requests);
    handles.assign(registration.resources().begin(),registration.resources().end());
    if(!catalog->exchange(registration)) invalid("Material lifecycle initialization became stale");
  }
  std::vector<ResourceHandle> inactive;inactive.reserve(retired.size());
  for(size_t row=0;row<retired.size();++row) if(retired[row]) inactive.push_back(handles[row]);
  if(!inactive.empty()) {
    auto transition=catalog->prepareRetirement(inactive,"Inherited retired material rows");
    if(!catalog->exchange(transition)) invalid("Inherited material phases became stale");
  }
  materialLifecycle_=std::move(catalog);materialLifecycleDomain_=domain;
  materialLifecycleHandles_.swap(handles);materialLifecycleNextSlot_=uint32_t(count);
  materialResourceDependencies_.clear();
}
bool SceneManager::materialIsActive(uint32_t runtime) const noexcept {
  if(runtime>=materialManager_.materialCount()) return false;
  // Initial loaders construct unpublished rows before the complete catalog is
  // bootstrapped. Every published mutation checks its nonnull exact owner.
  return !materialLifecycle_ || (runtime<materialLifecycleHandles_.size() && materialLifecycle_->active(materialLifecycleHandles_[runtime]));
}
bool SceneManager::authoredMaterialIsActive(int32_t source) const noexcept {
  return source>=0 && size_t(source)<sourceMaterialToRuntime_.size() && materialIsActive(sourceMaterialToRuntime_[size_t(source)]);
}
uint64_t SceneManager::materialIncarnation(uint32_t runtime) const noexcept {
  return materialLifecycle_ && runtime<materialLifecycleHandles_.size()?materialLifecycleHandles_[runtime].incarnation():0;
}
std::vector<uint8_t> SceneManager::materialRetirementMask() const {
  const auto count=materialManager_.materialCount();(void)materialLifecyclePreparationBytes(count);
  if(materialLifecycle_ && materialLifecycleHandles_.size()!=count) invalid("Material lifecycle row map is incomplete");
  std::vector<uint8_t> result(count);for(uint32_t runtime=0;runtime<count;++runtime) result[runtime]=!materialIsActive(runtime);return result;
}
std::vector<int32_t> SceneManager::retiredMaterialIndices() const {
  const auto mask=materialRetirementMask();std::vector<int32_t> result;
  result.reserve(std::count(mask.begin(),mask.end(),uint8_t(1)));
  for(size_t runtime=0;runtime<mask.size();++runtime) if(mask[runtime]) result.push_back(int32_t(runtime));return result;
}
container::util::ResourceCatalogSnapshot SceneManager::materialLifecycleInventory() const {
  return materialLifecycle_?materialLifecycle_->inventory():container::util::ResourceCatalogSnapshot{};
}
MaterialReferenceAudit SceneManager::auditUnusedMaterialReferences(uint32_t runtime,const SceneGraph& graph,
    const MaterialGenerationLimits& limits) const {
  if(!materialLifecycle_ || materialLifecycleHandles_.size()!=materialManager_.materialCount() || !materialIsActive(runtime))
    invalid("Material retirement requires the current active lifecycle incarnation");
  const auto source=sourceMaterialForRuntime(runtime);
  if(sourcePrimitiveImplicitMaterials_.size()>limits.references.maxReferences || materialManager_.materialCount()>limits.maxMaterialCount ||
      !limits.references.maxOwnedBytes || limits.references.maxOwnedBytes==UINT64_MAX ||
      sourcePrimitiveImplicitMaterials_.size()+2>limits.references.maxOwnedBytes/sizeof(uint32_t))
    invalid("Material retirement reference tables exceed their finite limits");
  std::vector<uint32_t> protectedMaterials;protectedMaterials.reserve(sourcePrimitiveImplicitMaterials_.size()+2);
  protectedMaterials.insert(protectedMaterials.end(),sourcePrimitiveImplicitMaterials_.begin(),sourcePrimitiveImplicitMaterials_.end());
  protectedMaterials.push_back(defaultMaterialIndex_);protectedMaterials.push_back(diagnosticMaterialIndex_);
  auto audit=AuditUnusedMaterialReferences({exportSource_,gltfModel_,graph,materialVariants_,
      {materialAssetIdentity(),sourceMaterialToRuntime_,uint32_t(materialCount()),uint32_t(primitiveRanges().size())},
      source<0?UINT32_MAX:uint32_t(source),runtime,protectedMaterials},nullptr,limits.references);
  const auto owners=materialSourceOwners(sourceMaterialToRuntime_,materialManager_.materialCount(),limits);
  if(!privateMaterialCacheSafe(exportMaterialSources_,exportSource_,sourceMaterialToRuntime_,owners,runtime,limits.references))
    invalid("A private material source cache has retained or unproved material references");
  return audit;
}
std::string SceneManager::materialRetirementBlockReason(uint32_t runtime,const SceneGraph& graph,
    const MaterialGenerationLimits& limits) const {
  try {(void)auditUnusedMaterialReferences(runtime,graph,limits);return {};}
  catch(const std::exception& error) {return std::string(std::string_view(error.what()).substr(0,512));}
}

int32_t SceneManager::sourceMaterialForRuntime(uint32_t runtime) const {
  ValidateSourceMaterialMapping(sourceMaterialToRuntime_,gltfModel_.materials.size(),materialManager_.materialCount());
  int32_t source=-1;
  for(size_t i=0;i<sourceMaterialToRuntime_.size();++i) if(sourceMaterialToRuntime_[i]==runtime) {
    if(source!=-1) invalid("Runtime material aliases multiple authored definitions");source=int32_t(i);
  }
  return source;
}
const tinygltf::Material& SceneManager::authoredMaterial(int32_t source) const {
  if(source<0 || size_t(source)>=gltfModel_.materials.size() || exportSource_.materials.size()!=gltfModel_.materials.size())
    invalid("Material edit requires an existing authored source material");
  return exportSource_.materials[size_t(source)];
}
GltfMaterialAuthoringPlan SceneManager::planMaterialEdit(int32_t source,const GltfMaterialPatch& patch,
    const GltfMaterialAuthoringLimits& limits) const {
  if(!authoredMaterialIsActive(source)) invalid("A retired material cannot be edited");
  const auto& authored=authoredMaterial(source);
  return PlanGltfMaterialEdit(GltfMaterialSourceView(exportSource_),GltfMaterialSourceView(gltfModel_),
      source,authored,gltfModel_.materials[size_t(source)],patch,limits);
}
GltfMaterialAuthoringPlan SceneManager::planMaterialDuplicate(int32_t source,const GltfMaterialPatch& patch,
    const GltfMaterialAuthoringLimits& limits) const {
  if(!authoredMaterialIsActive(source)) invalid("A retired material cannot be duplicated");
  const auto& authored=authoredMaterial(source);
  return PlanGltfMaterialDuplicate(GltfMaterialSourceView(exportSource_),GltfMaterialSourceView(gltfModel_),
      source,authored,gltfModel_.materials[size_t(source)],patch,limits);
}
GltfMaterialAuthoringPlan SceneManager::planMaterialMakeUnique(int32_t source,const GltfMaterialPatch& patch,
    const GltfMaterialAuthoringLimits& limits) const {
  if(!authoredMaterialIsActive(source)) invalid("A retired material cannot be made unique");
  const auto& authored=authoredMaterial(source);
  return PlanGltfMaterialMakeUnique(GltfMaterialSourceView(exportSource_),GltfMaterialSourceView(gltfModel_),
      source,authored,gltfModel_.materials[size_t(source)],patch,limits);
}
SceneManager::PreparedMaterialGeneration SceneManager::prepareRuntimeMaterialMakeUnique(
    uint32_t runtime,const GltfMaterialPatch& patch,const AnimationPlayer* player,const MaterialGenerationLimits& limits) const {
  const auto* origin=materialManager_.getMaterial(runtime);
  if(!origin || !materialIsActive(runtime) || sourceMaterialForRuntime(runtime)>=0) invalid("Runtime Make Unique requires an active source-less material");
  if(!limits.maxPreparedCpuBytes || limits.maxPreparedCpuBytes==UINT64_MAX ||
      !limits.authoring.maxOutputBytes || limits.authoring.maxOutputBytes==UINT64_MAX)
    invalid("Runtime materialization requires finite preparation capacities");
  constexpr uint64_t originBytes=sizeof(container::material::Material)+sizeof(RuntimeMaterialProvenance)+128;
  if(limits.maxPreparedCpuBytes<=originBytes) invalid("Runtime material origin exceeds its preparation budget");
  const auto original=*origin;
  const auto originalEpoch=generationEpoch_,originalRevision=materialRevision_,originalAsset=materialAssetIdentity();
  const auto originalOwner=materialResourceOwnerIdentity_;
  if(runtime>=runtimeMaterialProvenance_.size() || !runtimeMaterialProvenanceMatches(runtimeMaterialProvenance_[runtime],original))
    invalid("Runtime material source/texture provenance is unknown or stale");
  container::material::RuntimeMaterialSourceTextures authoredIds,evaluatedIds;
  const auto provenance=runtimeMaterialProvenance_[runtime];
  for(size_t slot=0;slot<provenance.slots.size();++slot) if(const auto& texture=provenance.slots[slot]) {
    if(!texture->evaluatedTexture()) invalid("Runtime texture identity is not present in the evaluated resource domain");
    if(texture->archiveTexture()>INT32_MAX || *texture->evaluatedTexture()>INT32_MAX) invalid("Runtime source texture identity exceeds glTF limits");
    authoredIds[slot]=int32_t(texture->archiveTexture());evaluatedIds[slot]=int32_t(*texture->evaluatedTexture());
  }
  const tinygltf::Material* retained=runtime<exportMaterialSources_.size() && exportMaterialSources_[runtime]
      ? &*exportMaterialSources_[runtime]:nullptr;
  container::material::RuntimeMaterialAuthoringLimits conversion;conversion.conversion.validation=limits.authoring;
  conversion.maxOutputBytes=std::min(conversion.maxOutputBytes,limits.maxPreparedCpuBytes-originBytes);
  const auto records=container::material::MaterializeRuntimeMaterial(original,authoredIds,evaluatedIds,
      {{exportSource_.textures.size(),exportSource_.extensionsUsed,archiveTextureResources_.color,archiveTextureResources_.data,textureManager_.textureCount()},retained},
      {{gltfModel_.textures.size(),gltfModel_.extensionsUsed,gltfTextureResources_.color,gltfTextureResources_.data,textureManager_.textureCount()},nullptr},conversion);
  if(records.ownedBytes()>=limits.maxPreparedCpuBytes-originBytes) invalid("Runtime materialization exceeds its aggregate preparation budget");
  auto preparedLimits=limits;preparedLimits.maxPreparedCpuBytes-=records.ownedBytes()+originBytes;
  // The plan and its detached-generation copy coexist with these two records.
  // Bound both before the planner makes its first selected-record copy.
  auto authoring=limits.authoring;authoring.maxOutputBytes=std::min(authoring.maxOutputBytes,preparedLimits.maxPreparedCpuBytes/2);
  if(!authoring.maxOutputBytes) invalid("Runtime material append has no remaining plan budget");
  preparedLimits.authoring=authoring;
  const auto plan=PlanGltfMaterialAppend(GltfMaterialSourceView(exportSource_),GltfMaterialSourceView(gltfModel_),
      records.authored(),records.evaluated(),patch,authoring);
  auto result=prepareMaterialGeneration(plan,player,preparedLimits);
  const auto* current=materialManager_.getMaterial(runtime);
  if(generationEpoch_!=originalEpoch || materialRevision_!=originalRevision || materialAssetIdentity()!=originalAsset ||
      materialResourceOwnerIdentity_!=originalOwner || !current || !container::material::RuntimeMaterialsEqual(original,*current) ||
      runtime>=runtimeMaterialProvenance_.size() || provenance!=runtimeMaterialProvenance_[runtime])
    invalid("Runtime material changed during materialization preparation");
  result.state_->runtimeOrigin=std::pair{runtime,original};
  return result;
}
GltfMaterialAuthoringPlan SceneManager::rebaseMaterialEdit(const GltfMaterialAuthoringPlan& command,
    GltfMaterialEditSide side,const GltfMaterialAuthoringLimits& limits) const {
  (void)GltfMaterialAuthoringContext(command);
  if(!command.sourceMaterial) invalid("Material history requires an existing source definition");
  const auto source=*command.sourceMaterial;
  if(!authoredMaterialIsActive(source)) invalid("Material history requires an active source material");
  return RebaseGltfMaterialEdit(command,side,authoredMaterial(source),gltfModel_.materials[size_t(source)],
      GltfMaterialSourceView(exportSource_),GltfMaterialSourceView(gltfModel_),limits);
}
GltfMaterialAuthoringPlan SceneManager::planMaterialTextureEdit(int32_t source,const GltfMaterialPatch& patch,
    const PreparedMaterialResources* resources,const GltfMaterialAuthoringLimits& limits) const {
  if(!authoredMaterialIsActive(source)) invalid("A retired material cannot edit texture bindings");
  if(resources && !canPrepareMaterialResources(*resources)) invalid("Texture resources changed before material planning");
  if(resources && resources->command().sourceMaterial()!=source) invalid("Texture resources belong to a different material");
  const auto& authored=authoredMaterial(source);
  return PlanGltfMaterialTextureEdit(GltfMaterialSourceView(exportSource_),GltfMaterialSourceView(gltfModel_),
      resources?resources->authoredSourceView():GltfMaterialSourceView(exportSource_),
      resources?resources->evaluatedSourceView():GltfMaterialSourceView(gltfModel_),
      source,authored,gltfModel_.materials[size_t(source)],patch,limits);
}
GltfMaterialAuthoringPlan SceneManager::rebaseMaterialTextureEdit(const GltfMaterialAuthoringPlan& command,
    GltfMaterialEditSide side,const PreparedMaterialResources* resources,const GltfMaterialAuthoringLimits& limits) const {
  const auto context=GltfMaterialAuthoringContext(command);
  if(!context.resourceTransition || !command.sourceMaterial) invalid("Texture history requires a proved texture transition");
  if(resources && !canPrepareMaterialResources(*resources)) invalid("Texture resources changed before history planning");
  const auto source=*command.sourceMaterial;
  if(!authoredMaterialIsActive(source)) invalid("Texture history requires an active source material");
  if(resources && resources->command().sourceMaterial()!=source) invalid("Texture history resources belong to a different material");
  return RebaseGltfMaterialTextureEdit(command,side,authoredMaterial(source),gltfModel_.materials[size_t(source)],
      GltfMaterialSourceView(exportSource_),GltfMaterialSourceView(gltfModel_),
      resources?resources->authoredSourceView():GltfMaterialSourceView(exportSource_),
      resources?resources->evaluatedSourceView():GltfMaterialSourceView(gltfModel_),limits);
}


GltfMaterialAuthoringPlan SceneManager::coalesceMaterialEdits(const GltfMaterialAuthoringPlan& cumulative,
    const GltfMaterialAuthoringPlan& next,const GltfMaterialAuthoringLimits& limits) const {
  return CoalesceGltfMaterialEdit(cumulative,next,GltfMaterialSourceView(exportSource_),GltfMaterialSourceView(gltfModel_),limits);
}

SceneManager::PreparedMaterialGeneration SceneManager::prepareMaterialGeneration(
    const GltfMaterialAuthoringPlan& plan,const AnimationPlayer* currentPlayer,const MaterialGenerationLimits& limits) const {
  return prepareMaterialGenerationImpl(plan,currentPlayer,limits,nullptr);
}
SceneManager::PreparedMaterialGeneration SceneManager::prepareMaterialGeneration(
    const GltfMaterialAuthoringPlan& plan,const AnimationPlayer* currentPlayer,PreparedMaterialResources&& resources,
    const MaterialGenerationLimits& limits) const {
  if(!resources || !canPrepareMaterialResources(resources)) invalid("Material resource generation is empty or stale");
  return prepareMaterialGenerationImpl(plan,currentPlayer,limits,&resources);
}
SceneManager::PreparedMaterialGeneration SceneManager::prepareMaterialGenerationImpl(
    const GltfMaterialAuthoringPlan& plan,const AnimationPlayer* currentPlayer,const MaterialGenerationLimits& limits,
    PreparedMaterialResources* resources,const MaterialAppendCommand* replayAppend) const try {
  (void)GltfMaterialAuthoringContext(plan);
  const auto authoredContext=resources?resources->authoredSourceView():GltfMaterialSourceView(exportSource_);
  const auto evaluatedContext=resources?resources->evaluatedSourceView():GltfMaterialSourceView(gltfModel_);
  const auto textureContext=resources?resources->conversionContext():container::material::GltfMaterialConversionContext{
      gltfModel_.textures.size(),gltfModel_.extensionsUsed,gltfTextureResources_.color,gltfTextureResources_.data,textureManager_.textureCount()};
  const auto images=resources?resources->textureImageOwnership():textureImageOwnership_;
  const bool transition=GltfMaterialAuthoringContext(plan).resourceTransition;
  const bool append=plan.kind==GltfMaterialPlanKind::Duplicate || plan.kind==GltfMaterialPlanKind::Append ||
      plan.kind==GltfMaterialPlanKind::MakeUnique;
  if(resources && (append || !transition)) invalid("Material resources require an existing-material texture transition");
  if(resources && (!plan.sourceMaterial || resources->command().sourceMaterial()!=*plan.sourceMaterial))
    invalid("Texture resources belong to a different material command");
  if((!append && plan.kind!=GltfMaterialPlanKind::Edit) || (!append && !plan.sourceMaterial) || !plan.changed)
    invalid("Material generation requires a changed proved material command");
  if(!materialLifecycle_ || materialLifecycleHandles_.size()!=materialManager_.materialCount() ||
      (plan.sourceMaterial && !authoredMaterialIsActive(*plan.sourceMaterial)) || (replayAppend && !append))
    invalid("Material generation requires current active lifecycle identities");
  if(!allocationManager_ || !pipelineManager_ || !deviceWrapper_ || descriptorPool_==VK_NULL_HANDLE ||
      descriptorSetLayout_==VK_NULL_HANDLE || materialBuffer_.buffer==VK_NULL_HANDLE ||
      textureMetadataBuffer_.buffer==VK_NULL_HANDLE || sceneClipStateBuffer_.buffer==VK_NULL_HANDLE ||
      generationEpoch_==UINT64_MAX || materialRevision_==UINT64_MAX || authoredMaterialRevision_==UINT64_MAX ||
      sceneRootCatalogIdentity_==UINT64_MAX) invalid("Live material resources or publication revisions are unavailable");
  if(!limits.maxPreparedCpuBytes || limits.maxPreparedCpuBytes==UINT64_MAX || !limits.maxMaterialBufferBytes ||
      limits.maxMaterialBufferBytes==UINT64_MAX || !limits.maxDescriptorFrames || limits.maxDescriptorFrames>1024 ||
      !limits.maxMaterialCount || limits.maxMaterialCount>INT32_MAX ||
      !limits.animation.maxOwnedBytes || limits.animation.maxOwnedBytes==UINT64_MAX)
    invalid("Material generation requires finite capacities");
  refreshTextureResourceLifecycle(container::util::ResourceRetirementPolicy::InvalidatePreparations,limits.maxPreparedCpuBytes,currentPlayer);
  Budget budget{limits.maxPreparedCpuBytes};
  const auto samplingBytes=container::material::TextureSamplingPlanStorageBytes(textureManager_.textureCount());
  budget.add(samplingBytes);
  if(!container::material::TextureImageOwnershipMatches(textureImageOwnership_,textureManager_,*allocationManager_) ||
      !container::material::TextureImageOwnershipValid(images,*allocationManager_))
    invalid("Material preparation requires complete old and prospective image pin coverage");
  if(!(resources?textureSamplingStateValidForResources():textureSamplingStateValid()))
    invalid("Material generation requires coherent owned texture sampling state");
  budget.add(materialManager_.materialCount()+size_t(append),sizeof(container::util::ResourceHandle));
  budget.add(4096); // One-row catalog registration/append authority/pins/transition and reasons.
  if(resources) budget.add(resources->ownedBytes());
  budget.add(sizeof(PreparedMaterialGeneration::State));budget.add(plan.peakOwnedBytes,2);
  auto validation=limits.authoring;
  if(!validation.maxOutputBytes || validation.maxOutputBytes==UINT64_MAX)
    invalid("Material validation requires a finite CPU budget");
  validation.maxOutputBytes=std::min(validation.maxOutputBytes,budget.available);
  budget.add(validation.maxOutputBytes); // Bound selected-binding validation scratch before it allocates.
  if(!append) {
    const auto source=*plan.sourceMaterial;
    if(transition)
      ValidateGltfMaterialEditTransition(plan,authoredMaterial(source),gltfModel_.materials[size_t(source)],
          GltfMaterialSourceView(exportSource_),GltfMaterialSourceView(gltfModel_),authoredContext,evaluatedContext,validation);
    else
      ValidateGltfMaterialEditBefore(plan,authoredMaterial(source),gltfModel_.materials[size_t(source)],
          GltfMaterialSourceView(exportSource_),GltfMaterialSourceView(gltfModel_),validation);
  } else {
    ValidateGltfMaterialAuthoringPlan(plan,GltfMaterialSourceView(exportSource_),GltfMaterialSourceView(gltfModel_),validation);
    if(plan.destinationMaterial<0 || size_t(plan.destinationMaterial)!=gltfModel_.materials.size() ||
        exportSource_.materials.size()!=gltfModel_.materials.size()) invalid("Material append source tables disagree");
    if(plan.sourceMaterial) {
      const auto source=*plan.sourceMaterial;
      if(!GltfMaterialRecordsEqual(authoredMaterial(source),plan.authoredBefore) ||
          !GltfMaterialRecordsEqual(gltfModel_.materials[size_t(source)],plan.evaluatedBefore))
        invalid("Material duplicate has stale authored/evaluated before records");
    }
  }
  ValidateSourceMaterialMapping(sourceMaterialToRuntime_,gltfModel_.materials.size(),materialManager_.materialCount());
  if(materialManager_.materialCount()>limits.maxMaterialCount || gltfModel_.materials.size()>limits.maxMaterialCount ||
      (append && (materialManager_.materialCount()==limits.maxMaterialCount || gltfModel_.materials.size()==limits.maxMaterialCount)))
    invalid("Material generation exceeds its identity table capacity");
  if(materialEntryOwners_.size()>materialManager_.materialCount()) invalid("Material ownership table exceeds the runtime material table");
  if(runtimeMaterialProvenance_.size()>materialManager_.materialCount()) invalid("Material source provenance exceeds its runtime table");
  const auto runtime=append?uint32_t(materialManager_.materialCount()):resolveLoadedMaterialIndex(*plan.sourceMaterial);
  if((!append && (sourceMaterialForRuntime(runtime)!=*plan.sourceMaterial || runtime>=exportMaterialSources_.size() ||
      runtime>=exportMaterialTextureBindings_.size() || !exportMaterialSources_[runtime] || !exportMaterialTextureBindings_[runtime])) ||
      exportMaterialSources_.size()>materialManager_.materialCount() || exportMaterialTextureBindings_.size()>materialManager_.materialCount() ||
      gpuMaterials_.size()!=materialManager_.materialCount() || gpuMaterials_.empty() || gpuMaterials_.size()>UINT32_MAX)
    invalid("Material generation source/runtime/export identity tables disagree");
  if(textureContext.colorResources.size()!=textureContext.textureCount || textureContext.dataResources.size()!=textureContext.textureCount ||
      textureContext.textureCount>1'000'000 || textureContext.runtimeResourceCount>1'000'000)
    invalid("Material source texture role tables are incomplete or exceed their limits");
  for(const auto roles:{textureContext.colorResources,textureContext.dataResources})
    for(const auto resource:roles) if(resource!=UINT32_MAX && resource>=textureContext.runtimeResourceCount)
      invalid("Material source texture role references an unavailable runtime resource");
  const size_t futureCount=gpuMaterials_.size()+size_t(append);
  budget.add(futureCount,sizeof(RuntimeMaterialProvenance));
  if(resources) budget.add(futureCount,sizeof(RuntimeMaterialProvenance));
  VkPhysicalDeviceProperties physical{};vkGetPhysicalDeviceProperties(deviceWrapper_->physicalDevice(),&physical);
  if(futureCount>limits.maxMaterialBufferBytes/sizeof(container::gpu::GpuMaterial) ||
      futureCount>physical.limits.maxStorageBufferRange/sizeof(container::gpu::GpuMaterial))
    invalid("Material generation exceeds its GPU table budget");
  if(currentPlayer) budget.add(limits.animation.maxOwnedBytes);
  else if(!gltfModel_.animations.empty()) invalid("Animated material publication requires its current animation player");
  budget.add(futureCount,sizeof(container::gpu::GpuMaterial)+sizeof(uint64_t)+sizeof(MaterialRenderProperties));
  budget.add(textureImageOwnership_->ownedBytes());
  if(images!=textureImageOwnership_) budget.add(images->ownedBytes());
  for(const auto declarations:std::array<std::span<const std::string>,3>{
      authoredContext.extensionsUsed,evaluatedContext.extensionsUsed,plan.extensionsToDeclare}) {
    if(declarations.size()>limits.authoring.maxMetadataValues) invalid("Material declarations exceed their traversal budget");
    budget.add(declarations.size(),sizeof(std::string)*4);for(const auto& name:declarations) budget.add(name.size(),4);
  }
  if(append) {
    // Charge every detached material table before the first prefix copy. No
    // Model, image bytes, geometry, channels or live vector capacity is copied.
    budget.add(sizeof(PreparedMaterialGeneration::State::AppendedTables));
    budget.add(futureCount,sizeof(container::material::Material)+sizeof(std::optional<tinygltf::Material>)+
        sizeof(std::optional<ImportedMaterialTextureBindings>));
    budget.add(gltfModel_.materials.size()+1,sizeof(uint32_t));
    budget.add(futureCount,sizeof(std::shared_ptr<const uint8_t>));
    budget.add(sizeof(MaterialAppendCommand::State)+64);
    budget.add(gltfModel_.materials.size(),sizeof(uint32_t));
    budget.add(container::material::TextureImageOwnershipStorageBytes(size_t(GltfMaterialTextureSlot::Count)*2));
    budget.add(GltfMaterialRecordStorageBytes(plan.authoredAfter,limits.authoring));
    budget.add(GltfMaterialRecordStorageBytes(plan.evaluatedAfter,limits.authoring));
    for(const auto* table:{&exportSource_.materials,&gltfModel_.materials})
      for(const auto& record:*table) budget.add(GltfMaterialRecordStorageBytes(record,limits.authoring));
    for(const auto& record:exportMaterialSources_)
      if(record) budget.add(GltfMaterialRecordStorageBytes(*record,limits.authoring));
    budget.add(GltfMaterialRecordStorageBytes(plan.authoredAfter,limits.authoring),2);
    budget.add(GltfMaterialRecordStorageBytes(plan.evaluatedAfter,limits.authoring),2);
    // The variant helper preflights its entire topology/metadata/work before
    // cloning. Reserve its finite coexistence cap in this aggregate budget.
    if(!limits.variantGrowth.maxOwnedBytes || limits.variantGrowth.maxOwnedBytes==UINT64_MAX)
      invalid("Material variant growth requires a finite storage capacity");
    budget.add(limits.variantGrowth.maxOwnedBytes);
  }
  const auto beforeResourceGeneration=resourceLifecycleGeneration();
  if(!beforeResourceGeneration || !beforeResourceGeneration->matches(textureResourceLifecycleView()) ||
      !materialResourceDependenciesValid(currentPlayer))
    invalid("Material preparation requires the exact current logical resource generation");
  budget.add(futureCount,2*sizeof(MaterialResourceDependencyStatePtr));
  std::optional<PreparedTextureResourceLifecycle> dependencyChange;
  if(!resources) {
    TextureResourceLifecycleLimits dependencyLimits;dependencyLimits.maxOwnedBytes=budget.available;
    dependencyChange.emplace(PrepareTextureResourceLifecycle(beforeResourceGeneration,textureResourceLifecycleView(),dependencyLimits));
    budget.add(dependencyChange->ownedBytes());
  }
  auto beforeReferences=RetainAllTextureResourceReferences(beforeResourceGeneration,
      container::util::ResourcePinClass::GpuReader,budget.available);budget.add(beforeReferences->ownedBytes());
  auto references=resources?retainSceneResourceReferences(resources,budget.available):
      dependencyChange->selectAll(container::util::ResourcePinClass::Preparation,budget.available);
  budget.add(references->ownedBytes());
  PreparedMaterialGeneration result;result.state_=std::make_unique<PreparedMaterialGeneration::State>();
  auto& s=*result.state_;s.owner=this;s.ownerIdentity=sceneRootOwnerIdentity_;s.device=deviceWrapper_;
  s.allocation=allocationManager_;s.pipelines=pipelineManager_;s.limits=limits;s.limits.authoring=validation;s.plan=plan;s.runtime=runtime;
  s.resourceTransition=transition;
  s.epoch=generationEpoch_;s.geometry=geometryRevision_;s.material=materialRevision_;s.authoredRevision=authoredMaterialRevision_;
  s.catalog=sceneRootCatalogIdentity_;s.materialCount=materialManager_.materialCount();s.sourceCount=gltfModel_.materials.size();
  s.resourceRevision=materialResourceRevision_;
  s.lifecycle=materialLifecycle_;s.lifecycleNextBefore=s.lifecycleNextAfter=materialLifecycleNextSlot_;
  s.expectedVacancy=materialTailVacancy_;s.ownersBefore=materialEntryOwners_.size();
  s.provenanceBefore=runtimeMaterialProvenance_.size();s.resourceOwner=materialResourceOwnerIdentity_;
  s.provenance.reserve(futureCount);s.provenance.assign(runtimeMaterialProvenance_.begin(),runtimeMaterialProvenance_.end());
  s.provenance.resize(futureCount);
  if(!append) s.previousProvenance=s.provenance[runtime];
  if(resources) {
    s.previousProvenanceRows=s.provenance;
    for(size_t i=0;i<s.provenance.size();++i) if(i!=runtime && s.provenance[i].known)
      s.provenance[i].slots=resources->forwardProvenance(s.provenance[i].slots,*materialManager_.getMaterial(uint32_t(i)));
  }
  s.authoredTextureCount=exportSource_.textures.size();s.textureCount=gltfModel_.textures.size();s.runtimeTextures=textureManager_.textureCount();
  s.layout=descriptorSetLayout_;s.expectedPool=descriptorPool_;s.expectedMaterial=materialBuffer_.buffer;
  s.textureMetadata=textureMetadataBuffer_.buffer;s.clip=sceneClipStateBuffer_.buffer;s.beforeImages=textureImageOwnership_;s.images=images;
  s.beforeResourceGeneration=beforeResourceGeneration;s.resourceGeneration=resources?resources->resourceLifecycleGeneration():dependencyChange->next();
  s.beforeReferences=std::move(beforeReferences);s.references=std::move(references);
  s.beforeDependencies=materialResourceDependencies_;s.dependencies=materialResourceDependencies_;s.dependencies.resize(futureCount);
  s.dependencyChange=std::move(dependencyChange);
  s.samplingFallback=textureSamplingFallback_;
  s.sampling=container::material::MakeTextureSamplingPlan(textureManager_,textureSamplingFallback_->resource,
      *allocationManager_,textureDescriptorCapacity_,materialSamplerDescriptorCapacity_,samplingBytes);
  s.expectedAuthoredDeclarations=exportSource_.extensionsUsed;
  s.expectedEvaluatedDeclarations=gltfModel_.extensionsUsed;
  s.authoredDeclarations.assign(authoredContext.extensionsUsed.begin(),authoredContext.extensionsUsed.end());
  s.evaluatedDeclarations.assign(evaluatedContext.extensionsUsed.begin(),evaluatedContext.extensionsUsed.end());
  s.descriptorCpuAvailable=budget.available;
  for(const auto& name:plan.extensionsToDeclare) for(auto* declarations:{&s.authoredDeclarations,&s.evaluatedDeclarations})
    if(std::ranges::find(*declarations,name)==declarations->end()) declarations->push_back(name);
  if(currentPlayer) {
    s.expectedPlayer=currentPlayer;s.selectedClip=currentPlayer->selectedClip();s.time=currentPlayer->time();s.speed=currentPlayer->speed();
    s.playing=currentPlayer->playing();s.loop=currentPlayer->loop();
    auto animationLimits=limits.animation;animationLimits.authoring=limits.authoring;
    s.player=currentPlayer->withMaterialEdits(gltfModel_,plan,animationLimits);
    // History's evaluated endpoint can come from a different clip/clock. The
    // detached sampler restores this material's tracked rests before applying
    // today's selected clip; untracked evaluated leaves remain independent.
    s.evaluated=s.player->evaluateMaterialRestEdit(size_t(plan.destinationMaterial),plan.evaluatedAfter,animationLimits);
  } else s.evaluated=plan.evaluatedAfter;
  s.captureCurves(gltfModel_,budget);
  container::material::GltfMaterialConversionLimits conversion;conversion.validation=limits.authoring;
  conversion.validation.maxOutputBytes=std::min(conversion.validation.maxOutputBytes,budget.available);
  s.converted=container::material::ConvertGltfMaterial(s.evaluated,
      {textureContext.textureCount,s.evaluatedDeclarations,textureContext.colorResources,textureContext.dataResources,
       textureContext.runtimeResourceCount,config_.experimentalGltfMaterials},
      container::material::GltfMaterialConversionPolicy::StrictGltf,conversion);
  s.exportRecord=plan.authoredAfter;s.exportBindings=s.converted.runtimeBindings;
  s.provenance[runtime]=resources?RuntimeMaterialProvenance{true,resources->makeProvenance(s.converted.value,plan.authoredAfter,s.evaluated)}:
      makeRuntimeMaterialProvenance(runtime,s.converted.value,plan.authoredAfter,s.evaluated);
  if(append) {
    if(replayAppend) {s.command=*replayAppend;s.replay=true;}
    else s.command.state_=std::make_shared<MaterialAppendCommand::State>();
    auto& command=*s.command.state_;
    if(!replayAppend) {
    command.owner=this;command.ownerIdentity=sceneRootOwnerIdentity_;command.asset=materialAssetIdentity();
    command.cookie=std::make_shared<const uint8_t>(0);command.source=plan.destinationMaterial;command.runtime=runtime;
    command.authored=plan.authoredAfter;command.evaluated=s.evaluated;
    command.prefixMapping=sourceMaterialToRuntime_;command.samplingFallback=textureSamplingFallback_;
    std::array<uint32_t,(size_t(GltfMaterialTextureSlot::Count)+experimentalTextureSlots)*2> selected{};size_t selectedCount{};
    for(const auto runtimeImage:s.converted.runtimeBindings.runtimeIndices) selected[selectedCount++]=runtimeImage;
    for(const auto& info:GltfMaterialTextureSlotRegistry()) {
      const auto binding=ReadGltfMaterialTextureBinding(command.authored,info.slot);if(binding.index<0) continue;
      const auto& roles=(info.srgb || binding.coupledSlot)?archiveTextureResources_.color:archiveTextureResources_.data;
      if(size_t(binding.index)>=roles.size() || roles[size_t(binding.index)]==UINT32_MAX)
        invalid("Appended material history has an unavailable authored image role");
      selected[selectedCount++]=roles[size_t(binding.index)];
    }
    if(config_.experimentalGltfMaterials) {
      for(const auto slot:s.converted.value.experimental.textureResources()) selected[selectedCount++]=slot;
      visitExperimentalMaterialTextures(command.authored,s.authoredDeclarations,archiveTextureResources_.color.size(),
          [&](uint32_t texture,bool color) {
            const auto& roles=color?archiveTextureResources_.color:archiveTextureResources_.data;
            if(texture>=roles.size() || roles[texture]==UINT32_MAX)
              invalid("Appended material history has an unavailable experimental image role");
            selected[selectedCount++]=roles[texture];
          });
    }
    command.images=container::material::SelectTextureImageOwnership(textureImageOwnership_,std::span(selected.data(),selectedCount),
        *allocationManager_,container::material::TextureImageOwnershipStorageBytes(selectedCount));
    MaterialResourceSelections logical{budget};
    logical.record(command.authored,container::util::ResourceDomainKind::Archive,beforeResourceGeneration);
    logical.record(command.evaluated,container::util::ResourceDomainKind::Evaluated,beforeResourceGeneration);
    logical.runtime(std::span(selected.data(),selectedCount));
    command.references=RetainTextureResourceReferences(beforeResourceGeneration,logical.rows,
        container::util::ResourcePinClass::History,budget.available,logical.opaque);
    budget.add(command.references->ownedBytes());
    command.provenance=s.provenance[runtime];
    command.bytes=sizeof(command)+64+command.prefixMapping.size()*sizeof(uint32_t)+
        command.images->ownedBytes()+command.references->ownedBytes()+
        GltfMaterialRecordStorageBytes(command.authored,limits.authoring)+GltfMaterialRecordStorageBytes(command.evaluated,limits.authoring);
    command.bytes+=4096;
    }
    s.appended=std::make_unique<PreparedMaterialGeneration::State::AppendedTables>();auto& tables=*s.appended;
    tables.variants=materialVariants_.withExtendedMaterialCount(gltfModel_.materials.size(),gltfModel_.materials.size()+1,limits.variantGrowth);
    tables.authored.reserve(gltfModel_.materials.size()+1);
    tables.authored.insert(tables.authored.end(),exportSource_.materials.begin(),exportSource_.materials.end());
    tables.authored.push_back(plan.authoredAfter);
    tables.evaluated.reserve(gltfModel_.materials.size()+1);
    tables.evaluated.insert(tables.evaluated.end(),gltfModel_.materials.begin(),gltfModel_.materials.end());
    tables.evaluated.push_back(s.evaluated);
    tables.exportedBefore=exportMaterialSources_.size();tables.bindingsBefore=exportMaterialTextureBindings_.size();
    tables.exported.reserve(futureCount);tables.exported.assign(exportMaterialSources_.begin(),exportMaterialSources_.end());
    tables.exported.resize(futureCount);tables.exported[runtime]=plan.authoredAfter;
    tables.bindings.reserve(futureCount);
    tables.bindings.assign(exportMaterialTextureBindings_.begin(),exportMaterialTextureBindings_.end());
    tables.bindings.resize(futureCount);tables.bindings[runtime]=s.converted.runtimeBindings;
    tables.mapping.reserve(gltfModel_.materials.size()+1);
    tables.mapping.insert(tables.mapping.end(),sourceMaterialToRuntime_.begin(),sourceMaterialToRuntime_.end());tables.mapping.push_back(runtime);
    tables.owners.reserve(futureCount);tables.owners.assign(materialEntryOwners_.begin(),materialEntryOwners_.end());
    tables.owners.resize(futureCount);tables.owners[runtime]=command.cookie;
    tables.runtime.materials_.reserve(futureCount);
    tables.runtime.materials_.insert(tables.runtime.materials_.end(),materialManager_.materials_.begin(),materialManager_.materials_.end());
    tables.runtime.materials_.push_back(s.converted.value);
  }
  s.lifecycleHandles=materialLifecycleHandles_;
  if(append) {
    auto& command=*s.command.state_;
    if(replayAppend) {
      if(!command.lifecycleAppend || command.lifecycleHandle.domain()!=materialLifecycleDomain_)
        invalid("Material append history has a foreign lifecycle identity");
      s.lifecycleTransition.emplace(materialLifecycle_->prepareAppendChange(command.lifecycleAppend,
          container::util::ResourceLifecyclePhase::Active));
    } else {
      if(materialLifecycleNextSlot_==UINT32_MAX) invalid("Material lifecycle internal identities are exhausted");
      const container::util::ResourceRegistrationRequest request{materialLifecycleDomain_,container::util::ResourceKind::Material,materialLifecycleNextSlot_};
      s.lifecycleRegistration.emplace(materialLifecycle_->prepareIssue(std::span(&request,1)));
      command.lifecycleHandle=s.lifecycleRegistration->resources().front();
      // The original append can suspend/resume today's exact active endpoint
      // without replacing an independent ordinary Material Delete receipt.
      command.lifecycleAppend=materialLifecycle_->retainAppendAuthority(*s.lifecycleRegistration,"Original owned material append");
      command.lifecycleHistory=materialLifecycle_->stagePin(*s.lifecycleRegistration,std::span(&command.lifecycleHandle,1),
          container::util::ResourcePinClass::History,"Owned material append history");
      s.lifecyclePreparation=materialLifecycle_->stagePin(*s.lifecycleRegistration,std::span(&command.lifecycleHandle,1),
          container::util::ResourcePinClass::Preparation,"Prepared appended material generation");
      ++s.lifecycleNextAfter;
    }
    s.lifecycleHandles.push_back(command.lifecycleHandle);
  } else {
    const auto& handle=materialLifecycleHandles_[runtime];
    s.lifecyclePreparation=materialLifecycle_->pin(std::span(&handle,1),container::util::ResourcePinClass::Preparation,
        "Prepared material generation");
  }
  const auto dependencyBytesBefore=resources?resources->ownedBytes():s.dependencyChange->ownedBytes();
  if(resources) {
    s.dependencies=stageResourceMaterialDependencies(*resources,runtime,s.lifecycleHandles[runtime],currentPlayer,budget.available);
    if(s.dependencies.size()!=futureCount) invalid("Paired resource dependency map has the wrong material shape");
    for(size_t row=0;row<s.dependencies.size();++row) if(row!=runtime && s.dependencies[row] && s.dependencies[row]!=s.beforeDependencies[row])
      budget.add(s.dependencies[row]->bytes);
  }
  const auto stageDependency=[&](uint32_t row,bool selected) {
    MaterialResourceDependencyInput input;input.runtime=row;input.material=s.lifecycleHandles[row];
    const auto& previous=row<s.beforeDependencies.size()?s.beforeDependencies[row]:MaterialResourceDependencyStatePtr{};
    input.source=selected?s.plan.destinationMaterial:(previous?previous->source:-1);
    input.value=selected?&s.converted.value:materialManager_.getMaterial(row);
    if(input.source>=0) {
      input.authored=selected?&s.plan.authoredAfter:&exportSource_.materials[size_t(input.source)];
      input.evaluated=selected?&s.evaluated:&gltfModel_.materials[size_t(input.source)];
      const auto* restPlayer=s.player?&*s.player:currentPlayer;
      if(restPlayer) {
        if(size_t(input.source)>=restPlayer->library().restMaterials.size()) invalid("Prospective material rest dependency is unavailable");
        input.rest=&restPlayer->library().restMaterials[size_t(input.source)];
      } else if(selected) input.rest=&s.plan.authoredAfter;
      else if(previous && previous->rest) input.rest=&*previous->rest;
      else input.rest=input.authored;
    }
    if(selected) {
      if(s.exportRecord) input.exported=&*s.exportRecord;
      if(s.exportBindings) input.exportBindings=&*s.exportBindings;
    } else {
      if(row<exportMaterialSources_.size() && exportMaterialSources_[row]) input.exported=&*exportMaterialSources_[row];
      if(row<exportMaterialTextureBindings_.size() && exportMaterialTextureBindings_[row]) input.exportBindings=&*exportMaterialTextureBindings_[row];
    }
    input.provenance=&s.provenance[row];
    s.dependencies[row]=stageMaterialResourceDependency(input,resources?nullptr:&*s.dependencyChange,resources,previous,budget.available);
    if(s.dependencies[row]!=previous) budget.add(s.dependencies[row]->bytes);
  };
  stageDependency(runtime,true);
  if(!resources) for(uint32_t row=0;row<s.materialCount;++row) if(row!=runtime && materialIsActive(row)) {
    const auto& old=s.beforeDependencies[row];
    if(!old || !old->provenance || *old->provenance!=s.provenance[row]) stageDependency(row,false);
  }
  if(resources) resources->sealLiveDependencies();else s.dependencyChange->seal();
  const auto dependencyBytesAfter=resources?resources->ownedBytes():s.dependencyChange->ownedBytes();
  if(dependencyBytesAfter>dependencyBytesBefore) budget.add(dependencyBytesAfter-dependencyBytesBefore);
  s.gpu.reserve(futureCount);s.gpu.assign(gpuMaterials_.begin(),gpuMaterials_.end());
  if(append) s.gpu.push_back(packMaterial(s.converted.value));else s.gpu[runtime]=packMaterial(s.converted.value);
  s.extended=std::ranges::any_of(s.gpu,[](const auto& m) {return m.anisotropyDiffuseFactors.x>0.f || m.anisotropyDiffuseFactors.z>0.f;});
  s.temporal.reserve(s.gpu.size());s.renderProperties.reserve(s.gpu.size());
  for(uint32_t i=0;i<s.gpu.size();++i) {
    s.gpu[i].anisotropyDiffuseFactors.w=s.extended?1.f:0.f;
    s.temporal.push_back(container::temporal::materialRevision(s.gpu[i]));
    s.renderProperties.push_back(i==runtime?properties(i,s.converted.value):materialRenderProperties(i));
  }
  s.routing=append || !sameRouting(materialRenderProperties(runtime),s.renderProperties[runtime]);
  const auto bytes=VkDeviceSize(s.gpu.size()*sizeof(container::gpu::GpuMaterial));
  s.buffer=createMaterialStorageBuffer(bytes);
  s.capacity=s.gpu.size();
  if(!s.buffer.allocation_info.pMappedData) invalid("Detached material buffer is not mapped");
  std::memcpy(s.buffer.allocation_info.pMappedData,s.gpu.data(),size_t(bytes));
  if(vmaFlushAllocation(allocationManager_->memoryManager()->allocator(),s.buffer.allocation,0,bytes)!=VK_SUCCESS)
    throw std::runtime_error("Failed to flush detached material buffer");
  s.descriptorCpuAvailable=budget.available;
  if(resources) s.resources.emplace(std::move(*resources));
  return result;
} catch(const std::length_error& error) {
  throw std::invalid_argument(error.what());
}

SceneManager::PreparedMaterialGeneration SceneManager::prepareMaterialTailChange(
    const MaterialAppendCommand& command,MaterialTailState target,const SceneGraph& graph,
    const AnimationPlayer* currentPlayer,const PreparedMaterialBindingChange* bindings,
    const MaterialGenerationLimits& limits) const try {
  using Phase=MaterialAppendCommand::State::Phase;
  if(!command || (target!=MaterialTailState::Present && target!=MaterialTailState::Detached))
    invalid("Material tail replay requires an owned command and a valid target");
  const auto& c=*command.state_;
  const bool remove=target==MaterialTailState::Detached;
  if(c.owner!=this || c.ownerIdentity!=sceneRootOwnerIdentity_ || c.asset!=materialAssetIdentity() ||
      c.samplingFallback!=textureSamplingFallback_ ||
      !materialLifecycle_ || materialLifecycleHandles_.size()!=materialManager_.materialCount() ||
      c.lifecycleHandle.domain()!=materialLifecycleDomain_ || !materialLifecycle_->contains(c.lifecycleHandle) ||
      !c.lifecycleAppend || c.lifecycleAppend.resources().size()!=1 || c.lifecycleAppend.resources().front()!=c.lifecycleHandle ||
      c.source<0 || c.phase!=(remove?Phase::Present:Phase::Detached) ||
      sourceMaterialToRuntime_.size()!=size_t(c.source)+size_t(remove) ||
      gltfModel_.materials.size()!=sourceMaterialToRuntime_.size() || exportSource_.materials.size()!=sourceMaterialToRuntime_.size() ||
      materialManager_.materialCount()!=size_t(c.runtime)+size_t(remove) || materialEntryOwners_.size()>materialManager_.materialCount() ||
      c.prefixMapping.size()!=size_t(c.source) ||
      !std::equal(c.prefixMapping.begin(),c.prefixMapping.end(),sourceMaterialToRuntime_.begin()))
    invalid("Material tail history belongs to a stale, foreign or non-tail generation");
  if(remove) {
    if(sourceMaterialToRuntime_.back()!=c.runtime || materialEntryOwners_.size()!=materialManager_.materialCount() ||
        materialLifecycleHandles_.back()!=c.lifecycleHandle || !materialIsActive(c.runtime) ||
        materialEntryOwners_.back()!=c.cookie ||
        exportMaterialSources_.size()!=materialManager_.materialCount() || !exportMaterialSources_.back() ||
        !GltfMaterialRecordsEqual(*exportMaterialSources_.back(),c.authored) ||
        !GltfMaterialRecordsEqual(exportSource_.materials.back(),c.authored) ||
        !GltfMaterialRecordsEqual(gltfModel_.materials.back(),c.evaluated))
      invalid("Material tail ownership or exact authored/evaluated records changed");
  } else if(materialTailVacancy_!=c.cookie)
    invalid("Material tail's detached slot was replaced by another history branch");
  if(!limits.maxPreparedCpuBytes || limits.maxPreparedCpuBytes==UINT64_MAX || command.ownedBytes()>=limits.maxPreparedCpuBytes)
    invalid("Material tail history exceeds its coexistence budget");

  refreshTextureResourceLifecycle(container::util::ResourceRetirementPolicy::InvalidatePreparations,limits.maxPreparedCpuBytes,currentPlayer);
  if(!c.references || !c.references->valid() || !c.references->resourceDependenciesActive() ||
      !c.references->resourceDependenciesMatch(resourceLifecycleGeneration()))
    invalid("Material tail history has retired or replaced resource dependencies");
  if(!remove) {
    auto effective=limits;effective.maxPreparedCpuBytes-=command.ownedBytes();
    if(!limits.authoring.maxOutputBytes || limits.authoring.maxOutputBytes==UINT64_MAX ||
        limits.authoring.maxOutputBytes>=effective.maxPreparedCpuBytes)
      invalid("Material tail redo cannot reserve its fresh plan before copying");
    effective.maxPreparedCpuBytes-=limits.authoring.maxOutputBytes;
    auto plan=PlanGltfMaterialAppend(GltfMaterialSourceView(exportSource_),GltfMaterialSourceView(gltfModel_),
        c.authored,c.evaluated,{},limits.authoring);
    auto result=prepareMaterialGenerationImpl(plan,currentPlayer,effective,nullptr,&command);
    auto& s=*result.state_;s.command=command;s.replay=true;s.futureVacancy=c.childVacancy;
    s.appended->owners.back()=c.cookie;
    return result;
  }

  if(!allocationManager_ || !pipelineManager_ || !deviceWrapper_ || descriptorPool_==VK_NULL_HANDLE ||
      descriptorSetLayout_==VK_NULL_HANDLE || materialBuffer_.buffer==VK_NULL_HANDLE ||
      textureMetadataBuffer_.buffer==VK_NULL_HANDLE || sceneClipStateBuffer_.buffer==VK_NULL_HANDLE ||
      generationEpoch_==UINT64_MAX || materialRevision_==UINT64_MAX || authoredMaterialRevision_==UINT64_MAX ||
      sceneRootCatalogIdentity_==UINT64_MAX) invalid("Live material resources or revisions are unavailable");
  if(!limits.maxMaterialBufferBytes || limits.maxMaterialBufferBytes==UINT64_MAX || !limits.maxDescriptorFrames ||
      limits.maxDescriptorFrames>1024 || !limits.maxMaterialCount || limits.maxMaterialCount>INT32_MAX ||
      materialManager_.materialCount()>limits.maxMaterialCount || !limits.animation.maxOwnedBytes ||
      limits.animation.maxOwnedBytes==UINT64_MAX || !limits.references.maxOwnedBytes ||
      limits.references.maxOwnedBytes==UINT64_MAX || !limits.variantGrowth.maxOwnedBytes ||
      limits.variantGrowth.maxOwnedBytes==UINT64_MAX) invalid("Material tail replay requires finite capacities");
  ValidateSourceMaterialMapping(sourceMaterialToRuntime_,gltfModel_.materials.size(),materialManager_.materialCount());
  validateGltfTextureResourceTable(gltfTextureResources_,gltfModel_.textures.size(),textureManager_.textureCount());
  if(gpuMaterials_.size()!=materialManager_.materialCount() || !c.runtime ||
      primitiveRanges().size()>=UINT32_MAX ||
      exportMaterialSources_.size()!=materialManager_.materialCount() ||
      exportMaterialTextureBindings_.size()!=materialManager_.materialCount())
    invalid("Material tail publication tables disagree or remove the required fallback");
  VkPhysicalDeviceProperties physical{};vkGetPhysicalDeviceProperties(deviceWrapper_->physicalDevice(),&physical);
  const auto futureCount=size_t(c.runtime),futureSource=size_t(c.source);
  if(runtimeMaterialProvenance_.size()>materialManager_.materialCount()) invalid("Material provenance exceeds its runtime table");
  if(futureCount>limits.maxMaterialBufferBytes/sizeof(container::gpu::GpuMaterial) ||
      futureCount>physical.limits.maxStorageBufferRange/sizeof(container::gpu::GpuMaterial))
    invalid("Material tail replay exceeds its GPU storage budget");
  Budget budget{limits.maxPreparedCpuBytes};
  const auto samplingBytes=container::material::TextureSamplingPlanStorageBytes(textureManager_.textureCount());
  budget.add(samplingBytes);
  if(!container::material::TextureImageOwnershipMatches(textureImageOwnership_,textureManager_,*allocationManager_) ||
      !container::material::TextureImageOwnershipValid(c.images,*allocationManager_))
    invalid("Material tail preparation requires retained selected images and complete live coverage");
  if(!textureSamplingStateValid()) invalid("Material tail requires coherent owned texture sampling state");
  budget.add(futureCount,sizeof(RuntimeMaterialProvenance));
  budget.add(futureCount,sizeof(container::util::ResourceHandle));budget.add(4096);
  budget.add(command.ownedBytes());budget.add(sizeof(PreparedMaterialGeneration::State));
  budget.add(sizeof(PreparedMaterialGeneration::State::AppendedTables));
  budget.add(futureCount,sizeof(container::material::Material)+sizeof(std::optional<tinygltf::Material>)+
      sizeof(std::optional<ImportedMaterialTextureBindings>)+sizeof(std::shared_ptr<const uint8_t>)+
      sizeof(container::gpu::GpuMaterial)+sizeof(uint64_t)+sizeof(MaterialRenderProperties));
  budget.add(futureSource,sizeof(uint32_t));
  budget.add(textureImageOwnership_->ownedBytes());
  budget.add(GltfMaterialRecordStorageBytes(c.evaluated,limits.authoring));
  for(size_t i=0;i<futureSource;++i) {
    budget.add(GltfMaterialRecordStorageBytes(exportSource_.materials[i],limits.authoring));
    budget.add(GltfMaterialRecordStorageBytes(gltfModel_.materials[i],limits.authoring));
  }
  for(size_t i=0;i<futureCount;++i) if(exportMaterialSources_[i])
    budget.add(GltfMaterialRecordStorageBytes(*exportMaterialSources_[i],limits.authoring));
  for(const auto* declarations:{&exportSource_.extensionsUsed,&gltfModel_.extensionsUsed}) {
    if(declarations->size()>limits.authoring.maxMetadataValues) invalid("Material tail declarations exceed their traversal limit");
    budget.add(declarations->size(),sizeof(std::string)*2);for(const auto& name:*declarations) budget.add(name.size(),2);
  }
  if(sourcePrimitiveImplicitMaterials_.size()>limits.references.maxReferences-std::min<uint64_t>(2,limits.references.maxReferences))
    invalid("Protected implicit material references exceed their table budget");
  budget.add(sourcePrimitiveImplicitMaterials_.size()+2,sizeof(uint32_t));
  budget.add(limits.references.maxOwnedBytes);budget.add(limits.variantGrowth.maxOwnedBytes);
  if(currentPlayer) budget.add(limits.animation.maxOwnedBytes);
  else if(!gltfModel_.animations.empty()) invalid("Animated material removal requires its current player");
  const auto resourceGeneration=resourceLifecycleGeneration();
  if(!resourceGeneration || !resourceGeneration->matches(textureResourceLifecycleView()) ||
      !materialResourceDependenciesValid(currentPlayer))
    invalid("Material tail requires the current logical resource generation");
  budget.add(materialManager_.materialCount()+futureCount,sizeof(MaterialResourceDependencyStatePtr));
  TextureResourceLifecycleLimits dependencyLimits;dependencyLimits.maxOwnedBytes=budget.available;
  auto dependencyChange=PrepareTextureResourceLifecycle(resourceGeneration,textureResourceLifecycleView(),dependencyLimits);
  budget.add(dependencyChange.ownedBytes());
  auto beforeReferences=RetainAllTextureResourceReferences(resourceGeneration,
      container::util::ResourcePinClass::GpuReader,budget.available);budget.add(beforeReferences->ownedBytes());
  auto references=dependencyChange.selectAll(container::util::ResourcePinClass::Preparation,budget.available);budget.add(references->ownedBytes());

  PreparedMaterialGeneration result;result.state_=std::make_unique<PreparedMaterialGeneration::State>();auto& s=*result.state_;
  s.owner=this;s.ownerIdentity=sceneRootOwnerIdentity_;s.device=deviceWrapper_;s.allocation=allocationManager_;s.pipelines=pipelineManager_;
  s.limits=limits;s.command=command;s.replay=true;s.removal=true;s.referenceGraph=&graph;
  s.plan.destinationMaterial=c.source;s.runtime=c.runtime;s.evaluated=c.evaluated;
  s.epoch=generationEpoch_;s.geometry=geometryRevision_;s.material=materialRevision_;s.authoredRevision=authoredMaterialRevision_;
  s.resourceRevision=materialResourceRevision_;
  s.lifecycle=materialLifecycle_;s.lifecycleNextBefore=s.lifecycleNextAfter=materialLifecycleNextSlot_;
  s.lifecycleHandles.assign(materialLifecycleHandles_.begin(),materialLifecycleHandles_.begin()+futureCount);
  s.lifecycleTransition.emplace(materialLifecycle_->prepareAppendChange(c.lifecycleAppend,
      container::util::ResourceLifecyclePhase::Retired,{},container::util::ResourceRetirementPolicy::InvalidatePreparations));
  s.catalog=sceneRootCatalogIdentity_;s.materialCount=materialManager_.materialCount();s.sourceCount=gltfModel_.materials.size();
  s.expectedVacancy=materialTailVacancy_;s.futureVacancy=c.cookie;s.ownersBefore=materialEntryOwners_.size();
  s.provenanceBefore=runtimeMaterialProvenance_.size();s.resourceOwner=materialResourceOwnerIdentity_;
  s.provenance.reserve(futureCount);
  s.provenance.assign(runtimeMaterialProvenance_.begin(),runtimeMaterialProvenance_.begin()+std::min(futureCount,runtimeMaterialProvenance_.size()));
  s.provenance.resize(futureCount);
  s.authoredTextureCount=exportSource_.textures.size();s.textureCount=gltfModel_.textures.size();s.runtimeTextures=textureManager_.textureCount();
  s.layout=descriptorSetLayout_;s.expectedPool=descriptorPool_;s.expectedMaterial=materialBuffer_.buffer;
  s.textureMetadata=textureMetadataBuffer_.buffer;s.clip=sceneClipStateBuffer_.buffer;s.beforeImages=textureImageOwnership_;s.images=textureImageOwnership_;
  s.beforeResourceGeneration=resourceGeneration;s.resourceGeneration=dependencyChange.next();
  s.beforeReferences=std::move(beforeReferences);s.references=std::move(references);
  s.beforeDependencies=materialResourceDependencies_;s.dependencies.assign(s.beforeDependencies.begin(),s.beforeDependencies.begin()+futureCount);
  s.dependencyChange.emplace(std::move(dependencyChange));
  s.samplingFallback=textureSamplingFallback_;
  s.sampling=container::material::MakeTextureSamplingPlan(textureManager_,textureSamplingFallback_->resource,
      *allocationManager_,textureDescriptorCapacity_,materialSamplerDescriptorCapacity_,samplingBytes);
  s.expectedAuthoredDeclarations=s.authoredDeclarations=exportSource_.extensionsUsed;
  s.expectedEvaluatedDeclarations=s.evaluatedDeclarations=gltfModel_.extensionsUsed;
  s.protectedMaterials.reserve(sourcePrimitiveImplicitMaterials_.size()+2);
  s.protectedMaterials.insert(s.protectedMaterials.end(),sourcePrimitiveImplicitMaterials_.begin(),sourcePrimitiveImplicitMaterials_.end());
  s.protectedMaterials.push_back(defaultMaterialIndex_);s.protectedMaterials.push_back(diagnosticMaterialIndex_);
  s.audit=AuditMaterialTailRemoval({exportSource_,gltfModel_,graph,materialVariants_,
      {materialAssetIdentity(),sourceMaterialToRuntime_,uint32_t(materialCount()),uint32_t(primitiveRanges().size())},
      uint32_t(c.source),c.runtime,s.protectedMaterials},bindings,limits.references);
  if(currentPlayer) {
    s.expectedPlayer=currentPlayer;s.selectedClip=currentPlayer->selectedClip();s.time=currentPlayer->time();s.speed=currentPlayer->speed();
    s.playing=currentPlayer->playing();s.loop=currentPlayer->loop();s.player=currentPlayer->withoutLastMaterialRest(gltfModel_,c.authored,limits.animation);
  }
  s.captureCurves(gltfModel_,budget);
  s.appended=std::make_unique<PreparedMaterialGeneration::State::AppendedTables>();auto& tables=*s.appended;
  tables.variants=materialVariants_.withRemovedLastMaterial(s.sourceCount,limits.variantGrowth);
  tables.authored.assign(exportSource_.materials.begin(),exportSource_.materials.begin()+futureSource);
  tables.evaluated.assign(gltfModel_.materials.begin(),gltfModel_.materials.begin()+futureSource);
  tables.exportedBefore=exportMaterialSources_.size();tables.bindingsBefore=exportMaterialTextureBindings_.size();
  tables.exported.assign(exportMaterialSources_.begin(),exportMaterialSources_.begin()+futureCount);
  tables.bindings.assign(exportMaterialTextureBindings_.begin(),exportMaterialTextureBindings_.begin()+futureCount);
  tables.mapping=c.prefixMapping;tables.owners.assign(materialEntryOwners_.begin(),materialEntryOwners_.begin()+futureCount);
  tables.runtime.materials_.assign(materialManager_.materials_.begin(),materialManager_.materials_.begin()+futureCount);
  s.gpu.assign(gpuMaterials_.begin(),gpuMaterials_.begin()+futureCount);
  s.extended=std::ranges::any_of(s.gpu,[](const auto& m){return m.anisotropyDiffuseFactors.x>0.f || m.anisotropyDiffuseFactors.z>0.f;});
  s.temporal.reserve(futureCount);s.renderProperties.reserve(futureCount);
  for(uint32_t i=0;i<futureCount;++i) {
    s.gpu[i].anisotropyDiffuseFactors.w=s.extended?1.f:0.f;
    s.temporal.push_back(container::temporal::materialRevision(s.gpu[i]));s.renderProperties.push_back(materialRenderProperties(i));
  }
  const auto dependencyBytesBefore=s.dependencyChange->ownedBytes();
  const std::array retiredDependency{s.beforeDependencies[c.runtime]->live};s.dependencyChange->releaseLiveOwners(retiredDependency);
  for(uint32_t row=0;row<futureCount;++row) if(materialIsActive(row)) {
    const auto& old=s.beforeDependencies[row];
    if(old->provenance && *old->provenance==s.provenance[row]) continue;
    MaterialResourceDependencyInput input;input.runtime=row;input.source=old->source;input.material=s.lifecycleHandles[row];
    input.value=materialManager_.getMaterial(row);input.provenance=&s.provenance[row];
    if(input.source>=0) {
      input.authored=&exportSource_.materials[size_t(input.source)];input.evaluated=&gltfModel_.materials[size_t(input.source)];
      if(s.player) input.rest=&s.player->library().restMaterials[size_t(input.source)];
      else if(old->rest) input.rest=&*old->rest;
    }
    if(row<exportMaterialSources_.size() && exportMaterialSources_[row]) input.exported=&*exportMaterialSources_[row];
    if(row<exportMaterialTextureBindings_.size() && exportMaterialTextureBindings_[row]) input.exportBindings=&*exportMaterialTextureBindings_[row];
    s.dependencies[row]=stageMaterialResourceDependency(input,&*s.dependencyChange,nullptr,old,budget.available);budget.add(s.dependencies[row]->bytes);
  }
  s.dependencyChange->seal();
  if(s.dependencyChange->ownedBytes()>dependencyBytesBefore) budget.add(s.dependencyChange->ownedBytes()-dependencyBytesBefore);
  s.routing=true;s.descriptorCpuAvailable=budget.available;
  const auto bytes=VkDeviceSize(futureCount*sizeof(container::gpu::GpuMaterial));
  s.buffer=createMaterialStorageBuffer(bytes);s.capacity=futureCount;
  if(!s.buffer.allocation_info.pMappedData) invalid("Detached tail material buffer is not mapped");
  std::memcpy(s.buffer.allocation_info.pMappedData,s.gpu.data(),size_t(bytes));
  if(vmaFlushAllocation(allocationManager_->memoryManager()->allocator(),s.buffer.allocation,0,bytes)!=VK_SUCCESS)
    throw std::runtime_error("Failed to flush detached tail material buffer");
  return result;
} catch(const std::length_error& error) {
  throw std::invalid_argument(error.what());
}

SceneManager::PreparedMaterialGeneration SceneManager::prepareMaterialRetirement(uint32_t runtime,
    const SceneGraph& graph,const AnimationPlayer* player,const MaterialGenerationLimits& limits) const try {
  if(!limits.maxPreparedCpuBytes || limits.maxPreparedCpuBytes==UINT64_MAX)
    invalid("Material retirement requires a finite CPU preparation budget");
  if(limits.maxPreparedCpuBytes<sizeof(PreparedMaterialGeneration::State)+8192)
    invalid("Material retirement has no room for bounded preparation scratch");
  refreshTextureResourceLifecycle(container::util::ResourceRetirementPolicy::InvalidatePreparations,limits.maxPreparedCpuBytes,player);
  (void)auditUnusedMaterialReferences(runtime,graph,limits);
  const auto source=sourceMaterialForRuntime(runtime);Budget budget{limits.maxPreparedCpuBytes};
  budget.add(sizeof(MaterialRetirementCommand::State)+4096);
  if(source>=0) {
    budget.add(GltfMaterialRecordStorageBytes(exportSource_.materials[size_t(source)],limits.authoring));
    budget.add(GltfMaterialRecordStorageBytes(gltfModel_.materials[size_t(source)],limits.authoring));
  }
  if(runtime<exportMaterialSources_.size() && exportMaterialSources_[runtime])
    budget.add(GltfMaterialRecordStorageBytes(*exportMaterialSources_[runtime],limits.authoring));
  const auto selected=retainMaterialImages(std::span(&runtime,1),budget.available);budget.add(selected->ownedBytes());
  const auto references=retainMaterialResourceReferences(std::span(&runtime,1),budget.available);budget.add(references->ownedBytes());
  MaterialRetirementCommand command;command.state_=std::make_shared<MaterialRetirementCommand::State>();auto& c=*command.state_;
  c.owner=this;c.ownerIdentity=sceneRootOwnerIdentity_;c.catalog=materialLifecycle_;c.handle=materialLifecycleHandles_[runtime];
  c.asset=materialAssetIdentity();c.source=source;c.runtime=runtime;c.value=*materialManager_.getMaterial(runtime);
  if(source>=0) {c.authored=exportSource_.materials[size_t(source)];c.evaluated=gltfModel_.materials[size_t(source)];}
  if(runtime<exportMaterialSources_.size()) c.exported=exportMaterialSources_[runtime];
  if(runtime<exportMaterialTextureBindings_.size()) c.bindings=exportMaterialTextureBindings_[runtime];
  if(runtime<runtimeMaterialProvenance_.size()) c.provenance=runtimeMaterialProvenance_[runtime];
  c.images=selected;c.references=references;c.samplingFallback=textureSamplingFallback_;c.bytes=limits.maxPreparedCpuBytes-budget.available;
  c.history=materialLifecycle_->pin(std::span(&c.handle,1),container::util::ResourcePinClass::History,"Arbitrary material retirement history");
  auto effective=limits;if(c.bytes>=effective.maxPreparedCpuBytes) invalid("Material retirement command exceeds its coexistence budget");
  effective.maxPreparedCpuBytes-=c.bytes;
  return prepareMaterialRetirementImpl(command,MaterialRetirementState::Retired,graph,player,effective);
} catch(const std::length_error& error) {
  throw std::invalid_argument(error.what());
}
SceneManager::PreparedMaterialGeneration SceneManager::prepareMaterialRetirementChange(const MaterialRetirementCommand& command,
    MaterialRetirementState target,const SceneGraph& graph,const AnimationPlayer* player,const MaterialGenerationLimits& limits) const {
  if(!command || !command.published() || !limits.maxPreparedCpuBytes || limits.maxPreparedCpuBytes==UINT64_MAX ||
      command.ownedBytes()>=limits.maxPreparedCpuBytes) invalid("Material retirement replay has no bounded published command");
  auto effective=limits;effective.maxPreparedCpuBytes-=command.ownedBytes();
  return prepareMaterialRetirementImpl(command,target,graph,player,effective);
}
SceneManager::PreparedMaterialGeneration SceneManager::prepareMaterialRetirementImpl(const MaterialRetirementCommand& command,
    MaterialRetirementState target,const SceneGraph& graph,const AnimationPlayer* player,const MaterialGenerationLimits& limits) const try {
  using namespace container::util;
  if(!command || (target!=MaterialRetirementState::Active && target!=MaterialRetirementState::Retired))
    invalid("Material retirement has an invalid command or phase");
  if(limits.maxPreparedCpuBytes<sizeof(PreparedMaterialGeneration::State)+8192)
    invalid("Material retirement has no room for bounded preparation scratch");
  const auto& c=*command.state_;const bool retire=target==MaterialRetirementState::Retired;
  if(c.owner!=this || c.ownerIdentity!=sceneRootOwnerIdentity_ || c.asset!=materialAssetIdentity() ||
      c.samplingFallback!=textureSamplingFallback_ ||
      !materialLifecycle_ || c.catalog!=materialLifecycle_ || c.handle.domain()!=materialLifecycleDomain_ ||
      materialLifecycleHandles_.size()!=materialManager_.materialCount() || c.runtime>=materialLifecycleHandles_.size() ||
      materialLifecycleHandles_[c.runtime]!=c.handle || c.retired==retire ||
      materialIsActive(c.runtime)!=retire || (c.source>=0 &&
      (size_t(c.source)>=sourceMaterialToRuntime_.size() || sourceMaterialToRuntime_[size_t(c.source)]!=c.runtime)) ||
      sourceMaterialForRuntime(c.runtime)!=c.source)
    invalid("Material retirement belongs to a stale, foreign or replaced incarnation");
  if(!allocationManager_ || !pipelineManager_ || !deviceWrapper_ || descriptorPool_==VK_NULL_HANDLE ||
      descriptorSetLayout_==VK_NULL_HANDLE || materialBuffer_.buffer==VK_NULL_HANDLE ||
      textureMetadataBuffer_.buffer==VK_NULL_HANDLE || sceneClipStateBuffer_.buffer==VK_NULL_HANDLE ||
      generationEpoch_==UINT64_MAX || materialRevision_==UINT64_MAX || authoredMaterialRevision_==UINT64_MAX)
    invalid("Live material retirement publication resources are unavailable");
  if(!limits.maxPreparedCpuBytes || limits.maxPreparedCpuBytes==UINT64_MAX || !limits.maxMaterialCount ||
      limits.maxMaterialCount>INT32_MAX || !limits.maxMaterialBufferBytes || limits.maxMaterialBufferBytes==UINT64_MAX ||
      !limits.maxDescriptorFrames || limits.maxDescriptorFrames>1024 || !limits.references.maxOwnedBytes ||
      limits.references.maxOwnedBytes==UINT64_MAX || !limits.animation.maxOwnedBytes || limits.animation.maxOwnedBytes==UINT64_MAX ||
      materialManager_.materialCount()>limits.maxMaterialCount || gpuMaterials_.size()!=materialManager_.materialCount() ||
      exportSource_.materials.size()!=gltfModel_.materials.size() || sourceMaterialToRuntime_.size()!=gltfModel_.materials.size())
    invalid("Material retirement tables or finite publication limits are invalid");
  refreshTextureResourceLifecycle(ResourceRetirementPolicy::InvalidatePreparations,limits.maxPreparedCpuBytes,player);
  if(!c.references || !c.references->valid() || !c.references->resourceDependenciesActive() ||
      !c.references->resourceDependenciesMatch(resourceLifecycleGeneration()))
    invalid("Material retirement history has retired or replaced resource dependencies");
  const tinygltf::Material tombstone;
  const auto* live=materialManager_.getMaterial(c.runtime);if(!live) invalid("Material retirement runtime row disappeared");
  if(retire && !container::material::RuntimeMaterialsEqual(*live,c.value)) invalid("Material retirement runtime record changed");
  Budget budget{limits.maxPreparedCpuBytes};budget.add(sizeof(PreparedMaterialGeneration::State)+8192);
  const auto samplingBytes=container::material::TextureSamplingPlanStorageBytes(textureManager_.textureCount());
  budget.add(samplingBytes);
  if(!container::material::TextureImageOwnershipMatches(textureImageOwnership_,textureManager_,*allocationManager_) ||
      !container::material::TextureImageOwnershipValid(c.images,*allocationManager_))
    invalid("Material retirement requires retained selected images and complete live coverage");
  if(!textureSamplingStateValid()) invalid("Material retirement requires coherent owned texture sampling state");
  budget.add(materialManager_.materialCount(),sizeof(RuntimeMaterialProvenance)+sizeof(ResourceHandle)+sizeof(int32_t)+
      sizeof(container::gpu::GpuMaterial)+sizeof(uint64_t)+sizeof(MaterialRenderProperties));
  budget.add(limits.references.maxOwnedBytes);budget.add(sourcePrimitiveImplicitMaterials_.size()+2,sizeof(uint32_t)*3);
  budget.add(textureImageOwnership_->ownedBytes());
  for(const auto* record:{&c.authored,&c.evaluated,&tombstone}) budget.add(GltfMaterialRecordStorageBytes(*record,limits.authoring),3);
  if(c.exported) budget.add(GltfMaterialRecordStorageBytes(*c.exported,limits.authoring),3);
  for(const auto* declarations:{&exportSource_.extensionsUsed,&gltfModel_.extensionsUsed}) {
    if(declarations->size()>limits.authoring.maxMetadataValues) invalid("Material retirement declaration table exceeds its bound");
    budget.add(declarations->size(),sizeof(std::string)*2);for(const auto& name:*declarations) budget.add(name.size(),2);
  }
  if(player) budget.add(limits.animation.maxOwnedBytes);else if(!gltfModel_.animations.empty())
    invalid("Animated material retirement requires the current animation player");
  auto conversion=limits.authoring;conversion.maxOutputBytes=std::min(conversion.maxOutputBytes,budget.available);
  if(!conversion.maxOutputBytes || conversion.maxOutputBytes==UINT64_MAX) invalid("Material retirement conversion requires a bounded budget");
  budget.add(conversion.maxOutputBytes);
  VkPhysicalDeviceProperties physical{};vkGetPhysicalDeviceProperties(deviceWrapper_->physicalDevice(),&physical);
  const auto count=materialManager_.materialCount();
  if(count>limits.maxMaterialBufferBytes/sizeof(container::gpu::GpuMaterial) ||
      count>physical.limits.maxStorageBufferRange/sizeof(container::gpu::GpuMaterial)) invalid("Material retirement exceeds its GPU table budget");
  const auto resourceGeneration=resourceLifecycleGeneration();
  if(!resourceGeneration || !resourceGeneration->matches(textureResourceLifecycleView()) || !materialResourceDependenciesValid(player))
    invalid("Material retirement requires the current logical resource generation");
  budget.add(count,2*sizeof(MaterialResourceDependencyStatePtr));
  TextureResourceLifecycleLimits dependencyLimits;dependencyLimits.maxOwnedBytes=budget.available;
  auto dependencyChange=PrepareTextureResourceLifecycle(resourceGeneration,textureResourceLifecycleView(),dependencyLimits);
  budget.add(dependencyChange.ownedBytes());
  auto beforeReferences=RetainAllTextureResourceReferences(resourceGeneration,ResourcePinClass::GpuReader,budget.available);
  budget.add(beforeReferences->ownedBytes());
  auto references=dependencyChange.selectAll(ResourcePinClass::Preparation,budget.available);budget.add(references->ownedBytes());
  PreparedMaterialGeneration result;result.state_=std::make_unique<PreparedMaterialGeneration::State>();auto& s=*result.state_;
  s.owner=this;s.ownerIdentity=sceneRootOwnerIdentity_;s.device=deviceWrapper_;s.allocation=allocationManager_;s.pipelines=pipelineManager_;
  s.limits=limits;s.retirement=command;s.retirementTarget=target;s.runtime=c.runtime;s.plan.destinationMaterial=c.source;
  s.referenceGraph=&graph;s.epoch=generationEpoch_;s.geometry=geometryRevision_;s.material=materialRevision_;
  s.authoredRevision=authoredMaterialRevision_;s.catalog=sceneRootCatalogIdentity_;s.materialCount=count;s.sourceCount=gltfModel_.materials.size();
  s.resourceRevision=materialResourceRevision_;s.resourceOwner=materialResourceOwnerIdentity_;s.provenanceBefore=runtimeMaterialProvenance_.size();
  s.expectedVacancy=materialTailVacancy_;s.ownersBefore=materialEntryOwners_.size();
  s.authoredTextureCount=exportSource_.textures.size();s.textureCount=gltfModel_.textures.size();s.runtimeTextures=textureManager_.textureCount();
  s.layout=descriptorSetLayout_;s.expectedPool=descriptorPool_;s.expectedMaterial=materialBuffer_.buffer;
  s.textureMetadata=textureMetadataBuffer_.buffer;s.clip=sceneClipStateBuffer_.buffer;s.descriptorCpuAvailable=budget.available;
  s.beforeImages=textureImageOwnership_;s.images=textureImageOwnership_;
  s.beforeResourceGeneration=resourceGeneration;s.resourceGeneration=dependencyChange.next();
  s.beforeReferences=std::move(beforeReferences);s.references=std::move(references);
  s.beforeDependencies=materialResourceDependencies_;s.dependencies=s.beforeDependencies;s.dependencyChange.emplace(std::move(dependencyChange));
  s.samplingFallback=textureSamplingFallback_;
  s.sampling=container::material::MakeTextureSamplingPlan(textureManager_,textureSamplingFallback_->resource,
      *allocationManager_,textureDescriptorCapacity_,materialSamplerDescriptorCapacity_,samplingBytes);
  s.lifecycle=materialLifecycle_;s.lifecycleNextBefore=s.lifecycleNextAfter=materialLifecycleNextSlot_;s.lifecycleHandles=materialLifecycleHandles_;
  s.runtimeOrigin=std::pair{c.runtime,*live};s.exportRowsBefore=exportMaterialSources_.size();s.bindingRowsBefore=exportMaterialTextureBindings_.size();
  if(c.runtime<exportMaterialSources_.size()) s.expectedExportRecord=exportMaterialSources_[c.runtime];
  if(c.runtime<exportMaterialTextureBindings_.size()) s.expectedExportBindings=exportMaterialTextureBindings_[c.runtime];
  if(c.source>=0) {
    s.plan.authoredBefore=exportSource_.materials[size_t(c.source)];s.plan.evaluatedBefore=gltfModel_.materials[size_t(c.source)];
    if(!GltfMaterialRecordsEqual(s.plan.authoredBefore,retire?c.authored:tombstone) ||
        !GltfMaterialRecordsEqual(s.plan.evaluatedBefore,retire?c.evaluated:tombstone)) invalid("Material retirement source records changed");
    s.plan.authoredAfter=retire?tombstone:c.authored;s.evaluated=retire?tombstone:c.evaluated;
  }
  if(retire) {
    if(bool(s.expectedExportRecord)!=bool(c.exported) || (c.exported && !GltfMaterialRecordsEqual(*s.expectedExportRecord,*c.exported)) ||
        bool(s.expectedExportBindings)!=bool(c.bindings) || (c.bindings && s.expectedExportBindings->runtimeIndices!=c.bindings->runtimeIndices))
      invalid("Material retirement private export record changed");
    if(c.exported) s.exportRecord=tombstone;
    if(c.bindings) s.exportBindings=ImportedMaterialTextureBindings{};
  } else {s.exportRecord=c.exported;s.exportBindings=c.bindings;}
  s.protectedMaterials.reserve(sourcePrimitiveImplicitMaterials_.size()+2);
  s.protectedMaterials.insert(s.protectedMaterials.end(),sourcePrimitiveImplicitMaterials_.begin(),sourcePrimitiveImplicitMaterials_.end());
  s.protectedMaterials.push_back(defaultMaterialIndex_);s.protectedMaterials.push_back(diagnosticMaterialIndex_);
  s.audit=AuditUnusedMaterialReferences({exportSource_,gltfModel_,graph,materialVariants_,
      {materialAssetIdentity(),sourceMaterialToRuntime_,uint32_t(count),uint32_t(primitiveRanges().size())},
      c.source<0?UINT32_MAX:uint32_t(c.source),c.runtime,s.protectedMaterials},nullptr,limits.references);
  s.cacheSources=materialSourceOwners(sourceMaterialToRuntime_,count,limits);
  if(!privateMaterialCacheSafe(exportMaterialSources_,exportSource_,sourceMaterialToRuntime_,s.cacheSources,c.runtime,limits.references))
    invalid("A private material cache has unproved incoming material references");
  s.expectedAuthoredDeclarations=s.authoredDeclarations=exportSource_.extensionsUsed;
  s.expectedEvaluatedDeclarations=s.evaluatedDeclarations=gltfModel_.extensionsUsed;
  if(player) {
    s.expectedPlayer=player;s.selectedClip=player->selectedClip();s.time=player->time();s.speed=player->speed();s.playing=player->playing();s.loop=player->loop();
    auto animationLimits=limits.animation;animationLimits.authoring=limits.authoring;
    if(c.source>=0) s.player=player->withUntrackedMaterialRest(gltfModel_,GltfMaterialSourceView(exportSource_),size_t(c.source),
        retire?c.authored:tombstone,retire?tombstone:c.authored,animationLimits);
  }
  s.captureCurves(gltfModel_,budget);
  container::material::GltfMaterialConversionLimits convertLimits;convertLimits.validation=conversion;
  s.converted=container::material::ConvertGltfMaterial(tombstone,
      {gltfModel_.textures.size(),gltfModel_.extensionsUsed,gltfTextureResources_.color,gltfTextureResources_.data,textureManager_.textureCount()},
      container::material::GltfMaterialConversionPolicy::StrictGltf,convertLimits);
  if(!retire) {
    if(!container::material::RuntimeMaterialsEqual(*live,s.converted.value) || bool(s.expectedExportRecord)!=bool(c.exported) ||
        (s.expectedExportRecord && !GltfMaterialRecordsEqual(*s.expectedExportRecord,tombstone)) ||
        bool(s.expectedExportBindings)!=bool(c.bindings) ||
        (s.expectedExportBindings && s.expectedExportBindings->runtimeIndices!=ImportedMaterialTextureBindings{}.runtimeIndices))
      invalid("Retired material tombstone records changed before restoration");
    s.converted.value=c.value;
  }
  s.provenance=runtimeMaterialProvenance_;s.provenance.resize(count);s.previousProvenance=s.provenance[c.runtime];
  if(!retire && s.previousProvenance!=RuntimeMaterialProvenance{true,{}})
    invalid("Retired material provenance changed before restoration");
  if(retire) s.provenance[c.runtime]=RuntimeMaterialProvenance{true,{}};
  else s.provenance[c.runtime]=c.provenance.known && c.source>=0?
      makeRuntimeMaterialProvenance(c.runtime,c.value,c.authored,c.evaluated):c.provenance;
  const auto slots=container::geometry::gltf::snapshotMaterialTextureBindings(s.converted.value).runtimeIndices;
  for(const auto texture:slots) if(texture!=UINT32_MAX && !textureManager_.getTexture(texture)) invalid("Material restore references an absent runtime texture");
  s.gpu=gpuMaterials_;s.gpu[c.runtime]=packMaterial(s.converted.value);
  s.extended=std::ranges::any_of(s.gpu,[](const auto& m){return m.anisotropyDiffuseFactors.x>0.f || m.anisotropyDiffuseFactors.z>0.f;});
  s.temporal.reserve(count);s.renderProperties.reserve(count);
  for(uint32_t row=0;row<count;++row) {
    s.gpu[row].anisotropyDiffuseFactors.w=s.extended?1.f:0.f;s.temporal.push_back(container::temporal::materialRevision(s.gpu[row]));
    s.renderProperties.push_back(row==c.runtime?properties(row,s.converted.value):materialRenderProperties(row));
  }
  s.routing=!sameRouting(materialRenderProperties(c.runtime),s.renderProperties[c.runtime]);
  if(c.published) s.lifecycleTransition.emplace(materialLifecycle_->prepareChange(c.authority,
      retire?ResourceLifecyclePhase::Retired:ResourceLifecyclePhase::Active,ResourceRetirementPolicy::InvalidatePreparations));
  else s.lifecycleTransition.emplace(materialLifecycle_->prepareRetirement(std::span(&c.handle,1),
      "Arbitrary unused material retired",ResourceRetirementPolicy::InvalidatePreparations));
  const auto dependencyBytesBefore=s.dependencyChange->ownedBytes();
  if(retire) {
    const std::array released{s.beforeDependencies[c.runtime]->live};s.dependencyChange->releaseLiveOwners(released);s.dependencies[c.runtime].reset();
  }
  for(uint32_t row=0;row<count;++row) if((row==c.runtime && !retire) || (row!=c.runtime && materialIsActive(row))) {
    const auto& old=s.beforeDependencies[row];
    if(row!=c.runtime && old->provenance && *old->provenance==s.provenance[row]) continue;
    MaterialResourceDependencyInput input;input.runtime=row;input.material=s.lifecycleHandles[row];
    input.source=row==c.runtime?c.source:old->source;input.value=row==c.runtime?&s.converted.value:materialManager_.getMaterial(row);
    input.provenance=&s.provenance[row];
    if(input.source>=0) {
      input.authored=row==c.runtime?&s.plan.authoredAfter:&exportSource_.materials[size_t(input.source)];
      input.evaluated=row==c.runtime?&s.evaluated:&gltfModel_.materials[size_t(input.source)];
      if(s.player) input.rest=&s.player->library().restMaterials[size_t(input.source)];
      else if(row==c.runtime) input.rest=input.authored;
      else if(old->rest) input.rest=&*old->rest;
    }
    if(row==c.runtime) {
      if(s.exportRecord) input.exported=&*s.exportRecord;
      if(s.exportBindings) input.exportBindings=&*s.exportBindings;
    } else {
      if(row<exportMaterialSources_.size() && exportMaterialSources_[row]) input.exported=&*exportMaterialSources_[row];
      if(row<exportMaterialTextureBindings_.size() && exportMaterialTextureBindings_[row]) input.exportBindings=&*exportMaterialTextureBindings_[row];
    }
    s.dependencies[row]=stageMaterialResourceDependency(input,&*s.dependencyChange,nullptr,old,budget.available);budget.add(s.dependencies[row]->bytes);
  }
  s.dependencyChange->seal();
  if(s.dependencyChange->ownedBytes()>dependencyBytesBefore) budget.add(s.dependencyChange->ownedBytes()-dependencyBytesBefore);
  s.descriptorCpuAvailable=budget.available;
  const auto bytes=VkDeviceSize(count*sizeof(container::gpu::GpuMaterial));
  s.buffer=createMaterialStorageBuffer(bytes);s.capacity=count;
  if(!s.buffer.allocation_info.pMappedData) invalid("Detached retirement material buffer is not mapped");
  std::memcpy(s.buffer.allocation_info.pMappedData,s.gpu.data(),size_t(bytes));
  if(vmaFlushAllocation(allocationManager_->memoryManager()->allocator(),s.buffer.allocation,0,bytes)!=VK_SUCCESS)
    throw std::runtime_error("Failed to flush detached retirement material buffer");
  return result;
} catch(const std::length_error& error) {
  throw std::invalid_argument(error.what());
}

void SceneManager::PreparedMaterialGeneration::prepareDescriptorFrames(
    std::span<const container::gpu::AllocatedBuffer> cameras,
    std::span<const container::gpu::AllocatedBuffer> objects,
    std::span<const container::gpu::AllocatedBuffer> auxiliaryObjects) {
  if(!*this) invalid("Material descriptor generation is empty or consumed");auto& s=*state_;const auto& owner=*s.owner;
  if(s.pool!=VK_NULL_HANDLE) invalid("Material descriptor generation has already been prepared");
  if(cameras.empty() || cameras.size()>s.limits.maxDescriptorFrames || objects.empty() ||
      (objects.size()!=1 && objects.size()!=cameras.size()) ||
      (!auxiliaryObjects.empty() && auxiliaryObjects.size()!=1 && auxiliaryObjects.size()!=cameras.size()))
    invalid("Material descriptor frame tables have incompatible shapes");
  if(!owner.materialGenerationMatches(*this,s.expectedPlayer))
    invalid("Material descriptor generation no longer belongs to the live asset");
  if(auxiliaryObjects.empty()) auxiliaryObjects=objects;
  for(const auto table:{cameras,objects,auxiliaryObjects}) for(const auto& buffer:table)
    if(buffer.buffer==VK_NULL_HANDLE) invalid("Material descriptor frame references an empty buffer");
  const auto frameCount=uint32_t(cameras.size()),sets=frameCount*2;
  Budget cpu{s.descriptorCpuAvailable};
  cpu.add(sets,2*sizeof(VkDescriptorSet)+sizeof(VkDescriptorSetLayout));
  cpu.add(container::material::TextureSamplingPlanStorageBytes(owner.textureManager_.textureCount()));
  cpu.add(owner.materialSamplerDescriptorCapacity_,sizeof(VkDescriptorImageInfo));
  cpu.add(16,sizeof(VkWriteDescriptorSet));
  for(const auto count:{owner.textureDescriptorCapacity_,owner.materialSamplerDescriptorCapacity_})
    if(count>UINT32_MAX/sets) invalid("Material descriptor pool counts overflow");
  const std::array sizes{
      VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,sets*2},
      VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,sets*3},
      VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,sets*owner.textureDescriptorCapacity_},
      VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_SAMPLER,sets*owner.materialSamplerDescriptorCapacity_}};
  VkDescriptorPoolCreateInfo create{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  create.maxSets=sets;create.poolSizeCount=uint32_t(sizes.size());create.pPoolSizes=sizes.data();
  if(vkCreateDescriptorPool(s.device->device(),&create,nullptr,&s.pool)!=VK_SUCCESS)
    throw std::runtime_error("Failed to create detached material descriptor pool");
  std::vector<VkDescriptorSetLayout> layouts(sets,s.layout);std::vector<VkDescriptorSet> allocated(sets);
  VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  allocate.descriptorPool=s.pool;allocate.descriptorSetCount=sets;allocate.pSetLayouts=layouts.data();
  if(vkAllocateDescriptorSets(s.device->device(),&allocate,allocated.data())!=VK_SUCCESS)
    throw std::runtime_error("Failed to allocate detached material descriptor sets");
  s.primary.assign(allocated.begin(),allocated.begin()+frameCount);s.auxiliary.assign(allocated.begin()+frameCount,allocated.end());
  for(uint32_t i=0;i<frameCount;++i) {
    const auto write=[&](VkDescriptorSet set,const container::gpu::AllocatedBuffer& object) {
      if(s.resources) s.resources->writeDescriptorSetContents(set,cameras[i],object,s.buffer,s.gpu.size());
      else owner.writeMaterialDescriptorSetContents(set,cameras[i],object,s.buffer,s.gpu.size());
    };
    write(s.primary[i],objects[std::min<size_t>(i,objects.size()-1)]);
    write(s.auxiliary[i],auxiliaryObjects[std::min<size_t>(i,auxiliaryObjects.size()-1)]);
  }
  s.descriptorsReady=true;
}

bool SceneManager::materialGenerationProvenanceMatches(const PreparedMaterialGeneration& token) const noexcept {
  if(!token) return false;const auto& s=*token.state_;
  if((s.command && !container::material::TextureImageOwnershipValid(s.command.state_->images,*allocationManager_)) ||
      (s.retirement && !container::material::TextureImageOwnershipValid(s.retirement.state_->images,*allocationManager_))) return false;
  for(const auto references:{s.command?s.command.state_->references:TextureResourceReferenceOwnerPtr{},
      s.retirement?s.retirement.state_->references:TextureResourceReferenceOwnerPtr{}})
    if(references && (!references->valid() || !references->resourceDependenciesActive() ||
        !references->resourceDependenciesMatch(resourceLifecycleGeneration()))) return false;
  if((s.command && !s.command.state_->references) || (s.retirement && !s.retirement.state_->references)) return false;
  if(s.resourceOwner!=materialResourceOwnerIdentity_ || s.provenanceBefore!=runtimeMaterialProvenance_.size() ||
      runtimeMaterialProvenance_.size()>materialManager_.materialCount() || s.provenance.size()!=s.gpu.size()) return false;
  const RuntimeMaterialProvenance unknown;
  const auto count=std::min(s.provenance.size(),materialManager_.materialCount());
  for(size_t i=0;i<count;++i) {
    const auto& live=i<runtimeMaterialProvenance_.size()?runtimeMaterialProvenance_[i]:unknown;
    const auto& expected=s.resources?s.previousProvenanceRows[i]:
        (!s.appended && i==s.runtime?s.previousProvenance:s.provenance[i]);
    if(live!=expected) return false;
  }
  if(s.command) {
    const auto& command=*s.command.state_;
    if(s.removal) {
      if(command.runtime>=runtimeMaterialProvenance_.size() || runtimeMaterialProvenance_[command.runtime]!=command.provenance) return false;
    } else if(command.runtime>=s.provenance.size() || s.provenance[command.runtime]!=command.provenance) return false;
  }
  if(!s.removal && !s.retirement && (s.runtime>=s.provenance.size() || !s.provenance[s.runtime].known ||
      !(s.resources?s.resources->provenanceMatches(s.provenance[s.runtime].slots,s.converted.value):
          runtimeMaterialProvenanceMatches(s.provenance[s.runtime],s.converted.value)))) return false;
  if(s.runtimeOrigin) {
    const auto* live=materialManager_.getMaterial(s.runtimeOrigin->first);
    if(!live || !container::material::RuntimeMaterialsEqual(*live,s.runtimeOrigin->second)) return false;
  }
  return true;
}
bool SceneManager::materialRetirementMatches(const PreparedMaterialGeneration& token) const noexcept {
  if(!token || !token.state_->retirement) return false;const auto& s=*token.state_;const auto& c=*s.retirement.state_;
  const bool retire=s.retirementTarget==MaterialRetirementState::Retired;
  if(c.owner!=this || c.ownerIdentity!=sceneRootOwnerIdentity_ || c.asset!=materialAssetIdentity() || c.catalog!=materialLifecycle_ ||
      !materialLifecycle_ || c.handle.domain()!=materialLifecycleDomain_ || c.runtime>=materialLifecycleHandles_.size() ||
      materialLifecycleHandles_[c.runtime]!=c.handle || c.retired==retire || materialIsActive(c.runtime)!=retire ||
      !s.lifecycleTransition || !materialLifecycle_->canExchange(*s.lifecycleTransition) ||
      s.exportRowsBefore!=exportMaterialSources_.size() || s.bindingRowsBefore!=exportMaterialTextureBindings_.size() ||
      !s.referenceGraph || !s.audit) return false;
  int32_t source=-1;
  for(size_t id=0;id<sourceMaterialToRuntime_.size();++id) if(sourceMaterialToRuntime_[id]==c.runtime) {
    if(source!=-1) return false;source=int32_t(id);
  }
  if(source!=c.source || (source>=0 &&
      (!GltfMaterialRecordsEqual(exportSource_.materials[size_t(source)],s.plan.authoredBefore) ||
       !GltfMaterialRecordsEqual(gltfModel_.materials[size_t(source)],s.plan.evaluatedBefore)))) return false;
  if(c.runtime<exportMaterialSources_.size()) {
    const auto& current=exportMaterialSources_[c.runtime];
    if(bool(current)!=bool(s.expectedExportRecord) || (current && !GltfMaterialRecordsEqual(*current,*s.expectedExportRecord))) return false;
  } else if(s.expectedExportRecord) return false;
  if(c.runtime<exportMaterialTextureBindings_.size()) {
    const auto& current=exportMaterialTextureBindings_[c.runtime];
    if(bool(current)!=bool(s.expectedExportBindings) || (current && current->runtimeIndices!=s.expectedExportBindings->runtimeIndices)) return false;
  } else if(s.expectedExportBindings) return false;
  if(!CanRetireUnusedMaterial({exportSource_,gltfModel_,*s.referenceGraph,materialVariants_,
      {materialAssetIdentity(),sourceMaterialToRuntime_,uint32_t(materialCount()),uint32_t(primitiveRanges().size())},
      source<0?UINT32_MAX:uint32_t(source),c.runtime,s.protectedMaterials},*s.audit,MaterialReferencePhase::BeforeBindings) ||
      !privateMaterialCacheSafe(exportMaterialSources_,exportSource_,sourceMaterialToRuntime_,s.cacheSources,c.runtime,s.limits.references)) return false;
  if(s.provenance[s.runtime].known && !runtimeMaterialProvenanceMatches(s.provenance[s.runtime],s.converted.value)) return false;
  return true;
}
bool SceneManager::materialGenerationMatches(const PreparedMaterialGeneration& token,const AnimationPlayer* player) const noexcept {
  if(!token || !materialGenerationProvenanceMatches(token)) return false;const auto& s=*token.state_;
  if(!s.dependenciesMatch(player)) return false;
  if(s.beforeResourceGeneration!=resourceLifecycleGeneration() || !s.beforeResourceGeneration ||
      !s.beforeResourceGeneration->matches(textureResourceLifecycleView()) ||
      !s.beforeReferences || !s.beforeReferences->valid() || !s.references || !s.references->valid() || !s.resourceGeneration) return false;
  if(s.beforeImages!=textureImageOwnership_ ||
      !container::material::TextureImageOwnershipMatches(s.beforeImages,textureManager_,*allocationManager_) ||
      !container::material::TextureImageOwnershipValid(s.images,*allocationManager_) ||
      (s.resources?s.images!=s.resources->textureImageOwnership():s.images!=s.beforeImages)) return false;
  if(s.samplingFallback!=textureSamplingFallback_ ||
      !(s.resources?textureSamplingStateValidForResources():textureSamplingStateValid()) ||
      !container::material::TextureSamplingPlanMatches(s.sampling,textureManager_,textureSamplingFallback_->resource,*allocationManager_)) return false;
  if(s.resourceRevision!=materialResourceRevision_ || (s.resources && !canExchangeMaterialResources(*s.resources))) return false;
  if(s.owner!=this || s.ownerIdentity!=sceneRootOwnerIdentity_ || s.epoch!=generationEpoch_ || s.geometry!=geometryRevision_ ||
      s.material!=materialRevision_ || s.authoredRevision!=authoredMaterialRevision_ || s.catalog!=sceneRootCatalogIdentity_ ||
      s.ownersBefore!=materialEntryOwners_.size() || s.expectedVacancy!=materialTailVacancy_ ||
      s.expectedPlayer!=player || s.sourceCount!=gltfModel_.materials.size() || s.sourceCount!=exportSource_.materials.size() ||
      s.materialCount!=materialManager_.materialCount() || s.sourceCount!=sourceMaterialToRuntime_.size() ||
      s.authoredTextureCount!=exportSource_.textures.size() || s.textureCount!=gltfModel_.textures.size() ||
      s.runtimeTextures!=textureManager_.textureCount() || s.expectedPool!=descriptorPool_ || s.layout!=descriptorSetLayout_ ||
      s.expectedMaterial!=materialBuffer_.buffer || s.textureMetadata!=textureMetadataBuffer_.buffer || s.clip!=sceneClipStateBuffer_.buffer ||
      s.expectedAuthoredDeclarations!=exportSource_.extensionsUsed || s.expectedEvaluatedDeclarations!=gltfModel_.extensionsUsed)
    return false;
  if(!materialLifecycle_ || s.lifecycle!=materialLifecycle_ || s.lifecycleNextBefore!=materialLifecycleNextSlot_ ||
      materialLifecycleHandles_.size()!=s.materialCount ||
      (s.lifecycleRegistration && !materialLifecycle_->canExchange(*s.lifecycleRegistration)) ||
      (s.lifecycleTransition && !materialLifecycle_->canExchange(*s.lifecycleTransition))) return false;
  for(size_t i=0;i<std::min(materialLifecycleHandles_.size(),s.lifecycleHandles.size());++i)
    if(materialLifecycleHandles_[i]!=s.lifecycleHandles[i]) return false;
  if(s.retirement) {
    if(!materialRetirementMatches(token)) return false;
    if(player && (s.selectedClip!=player->selectedClip() || std::bit_cast<uint64_t>(s.time)!=std::bit_cast<uint64_t>(player->time()) ||
        std::bit_cast<uint64_t>(s.speed)!=std::bit_cast<uint64_t>(player->speed()) || s.playing!=player->playing() || s.loop!=player->loop())) return false;
    return true;
  }
  if(s.command) {
    const auto& c=*s.command.state_;using Phase=MaterialAppendCommand::State::Phase;
    const auto expected=s.replay?(s.removal?Phase::Present:Phase::Detached):Phase::Unpublished;
    if(c.owner!=this || c.ownerIdentity!=sceneRootOwnerIdentity_ || c.asset!=materialAssetIdentity() || c.phase!=expected ||
        c.source!=s.plan.destinationMaterial || c.runtime!=s.runtime ||
        c.lifecycleHandle.domain()!=materialLifecycleDomain_ ||
        (s.removal && (c.runtime>=materialLifecycleHandles_.size() || materialLifecycleHandles_[c.runtime]!=c.lifecycleHandle)) ||
        (!s.removal && (s.lifecycleHandles.empty() || s.lifecycleHandles.back()!=c.lifecycleHandle))) return false;
    if(s.removal) {
      if(materialEntryOwners_.empty() || materialEntryOwners_.back()!=c.cookie || sourceMaterialToRuntime_.back()!=c.runtime ||
          !GltfMaterialRecordsEqual(exportSource_.materials.back(),c.authored) ||
          !GltfMaterialRecordsEqual(gltfModel_.materials.back(),c.evaluated) ||
          !exportMaterialSources_.back() || !GltfMaterialRecordsEqual(*exportMaterialSources_.back(),c.authored) ||
          !s.referenceGraph || !s.audit ||
          !CanRemoveMaterialTail({exportSource_,gltfModel_,*s.referenceGraph,materialVariants_,
              {materialAssetIdentity(),sourceMaterialToRuntime_,uint32_t(materialCount()),uint32_t(primitiveRanges().size())},
              uint32_t(c.source),c.runtime,s.protectedMaterials},*s.audit,MaterialTailAuditPhase::BeforeBindings)) return false;
    } else if(s.replay && materialTailVacancy_!=c.cookie) return false;
  }
  if(s.appended) {
    const auto& tables=*s.appended;
    const auto targetSource=s.removal?s.sourceCount-1:s.sourceCount+1;
    const auto targetRuntime=s.removal?s.materialCount-1:s.materialCount+1;
    if(s.runtime!=(s.removal?s.materialCount-1:s.materialCount) || size_t(s.plan.destinationMaterial)!=(s.removal?s.sourceCount-1:s.sourceCount) ||
        tables.authored.size()!=targetSource || tables.evaluated.size()!=targetSource ||
        tables.mapping.size()!=targetSource || (!s.removal && tables.mapping.back()!=s.runtime) || tables.owners.size()!=targetRuntime ||
        tables.exportedBefore!=exportMaterialSources_.size() || tables.bindingsBefore!=exportMaterialTextureBindings_.size() ||
        materialVariants_.materialCount()!=s.sourceCount) return false;
    for(size_t i=0;i<std::min(s.sourceCount,targetSource);++i)
      if(tables.mapping[i]!=sourceMaterialToRuntime_[i] ||
          !GltfMaterialRecordsEqual(tables.authored[i],exportSource_.materials[i]) ||
          !GltfMaterialRecordsEqual(tables.evaluated[i],gltfModel_.materials[i])) return false;
    for(size_t i=0;i<std::min(tables.exported.size(),exportMaterialSources_.size());++i) {
      if(bool(tables.exported[i])!=bool(exportMaterialSources_[i])) return false;
      if(tables.exported[i] && !GltfMaterialRecordsEqual(*tables.exported[i],*exportMaterialSources_[i])) return false;
    }
    for(size_t i=0;i<std::min(tables.bindings.size(),exportMaterialTextureBindings_.size());++i) {
      if(bool(tables.bindings[i])!=bool(exportMaterialTextureBindings_[i])) return false;
      if(tables.bindings[i] && tables.bindings[i]->runtimeIndices!=exportMaterialTextureBindings_[i]->runtimeIndices) return false;
    }
    for(size_t i=0;i<std::min(s.materialCount,targetRuntime);++i)
      if(tables.owners[i]!=(i<materialEntryOwners_.size()?materialEntryOwners_[i]:std::shared_ptr<const uint8_t>{})) return false;
  } else if(s.plan.destinationMaterial<0 || size_t(s.plan.destinationMaterial)>=sourceMaterialToRuntime_.size() ||
      s.runtime!=sourceMaterialToRuntime_[size_t(s.plan.destinationMaterial)]) return false;
  if(player && (s.selectedClip!=player->selectedClip() || std::bit_cast<uint64_t>(s.time)!=std::bit_cast<uint64_t>(player->time()) ||
      std::bit_cast<uint64_t>(s.speed)!=std::bit_cast<uint64_t>(player->speed()) || s.playing!=player->playing() || s.loop!=player->loop())) return false;
  for(size_t slot=0;!s.removal && slot<s.converted.sourceTextures.size();++slot) if(const auto texture=s.converted.sourceTextures[slot];texture>=0) {
    const auto context=s.resources?s.resources->conversionContext():container::material::GltfMaterialConversionContext{
        gltfModel_.textures.size(),gltfModel_.extensionsUsed,gltfTextureResources_.color,gltfTextureResources_.data,textureManager_.textureCount()};
    const auto role=container::material::GltfMaterialTextureUsesColorResource(
        GltfMaterialTextureSlot(slot),s.converted.value)?context.colorResources:context.dataResources;
    if(size_t(texture)>=role.size() || role[size_t(texture)]!=s.converted.runtimeBindings.runtimeIndices[slot]) return false;
  }
  return s.authoringMatches();
}
bool SceneManager::canExchangeMaterialGeneration(const PreparedMaterialGeneration& token,const AnimationPlayer* player) const noexcept {
  return materialGenerationMatches(token,player) && token.state_->descriptorsReady;
}

void SceneManager::exchangeMaterialGeneration(PreparedMaterialGeneration& token,AnimationPlayer* player) noexcept {
  if(!token || token.state_->owner!=this || !materialGenerationProvenanceMatches(token)) return;auto& s=*token.state_;
  if(!s.dependenciesMatch(player) || !s.authoringMatches()) return;
  if(s.beforeResourceGeneration!=resourceLifecycleGeneration() || !s.beforeResourceGeneration ||
      !s.beforeResourceGeneration->matches(textureResourceLifecycleView()) ||
      !s.beforeReferences || !s.beforeReferences->valid() || !s.references || !s.references->valid() || !s.resourceGeneration) return;
  if(s.beforeImages!=textureImageOwnership_ ||
      !container::material::TextureImageOwnershipMatches(s.beforeImages,textureManager_,*allocationManager_) ||
      !container::material::TextureImageOwnershipValid(s.images,*allocationManager_) ||
      (s.resources?s.images!=s.resources->textureImageOwnership():s.images!=s.beforeImages)) return;
  if(s.samplingFallback!=textureSamplingFallback_ ||
      !(s.resources?textureSamplingStateValidForResources():textureSamplingStateValid()) ||
      !container::material::TextureSamplingPlanMatches(s.sampling,textureManager_,textureSamplingFallback_->resource,*allocationManager_)) return;
  if(s.resourceRevision!=materialResourceRevision_ || (s.resources && !canExchangeMaterialResources(*s.resources))) return;
  if(!s.descriptorsReady || s.expectedPlayer!=player || s.ownerIdentity!=sceneRootOwnerIdentity_ ||
      s.epoch!=generationEpoch_ || s.material!=materialRevision_ || s.geometry!=geometryRevision_ ||
      s.authoredRevision!=authoredMaterialRevision_ || s.catalog!=sceneRootCatalogIdentity_ ||
      s.ownersBefore!=materialEntryOwners_.size() || s.expectedVacancy!=materialTailVacancy_ ||
      s.sourceCount!=gltfModel_.materials.size() || s.sourceCount!=exportSource_.materials.size() ||
      s.materialCount!=materialManager_.materialCount() || s.sourceCount!=sourceMaterialToRuntime_.size() ||
      s.authoredTextureCount!=exportSource_.textures.size() || s.textureCount!=gltfModel_.textures.size() ||
      s.runtimeTextures!=textureManager_.textureCount() || s.expectedPool!=descriptorPool_ || s.layout!=descriptorSetLayout_ ||
      s.expectedMaterial!=materialBuffer_.buffer || s.textureMetadata!=textureMetadataBuffer_.buffer ||
      s.clip!=sceneClipStateBuffer_.buffer) return;
  if(!materialLifecycle_ || s.lifecycle!=materialLifecycle_ || s.lifecycleNextBefore!=materialLifecycleNextSlot_ ||
      materialLifecycleHandles_.size()!=s.materialCount ||
      (s.lifecycleRegistration && !materialLifecycle_->canExchange(*s.lifecycleRegistration)) ||
      (s.lifecycleTransition && !materialLifecycle_->canExchange(*s.lifecycleTransition))) return;
  for(size_t i=0;i<std::min(materialLifecycleHandles_.size(),s.lifecycleHandles.size());++i)
    if(materialLifecycleHandles_[i]!=s.lifecycleHandles[i]) return;
  if(player && (s.selectedClip!=player->selectedClip() || std::bit_cast<uint64_t>(s.time)!=std::bit_cast<uint64_t>(player->time()) ||
      std::bit_cast<uint64_t>(s.speed)!=std::bit_cast<uint64_t>(player->speed()) || s.playing!=player->playing() || s.loop!=player->loop())) return;
  if(s.expectedAuthoredDeclarations!=exportSource_.extensionsUsed || s.expectedEvaluatedDeclarations!=gltfModel_.extensionsUsed) return;
  if(s.appended) {
    const auto& tables=*s.appended;
    for(size_t i=0;i<std::min(tables.authored.size(),exportSource_.materials.size());++i)
      if(!GltfMaterialRecordsEqual(tables.authored[i],exportSource_.materials[i]) ||
          !GltfMaterialRecordsEqual(tables.evaluated[i],gltfModel_.materials[i])) return;
  } else if(s.retirement) {
    if(!materialRetirementMatches(token)) return;
  } else if(s.plan.destinationMaterial<0 || size_t(s.plan.destinationMaterial)>=exportSource_.materials.size() ||
      s.runtime!=sourceMaterialToRuntime_[size_t(s.plan.destinationMaterial)] ||
      !GltfMaterialRecordsEqual(exportSource_.materials[size_t(s.plan.destinationMaterial)],s.plan.authoredBefore) ||
      !GltfMaterialRecordsEqual(gltfModel_.materials[size_t(s.plan.destinationMaterial)],s.plan.evaluatedBefore)) {
    return;
  }
  if(s.command) {
    const auto& c=*s.command.state_;using Phase=MaterialAppendCommand::State::Phase;
    if(c.phase!=(s.replay?(s.removal?Phase::Present:Phase::Detached):Phase::Unpublished) ||
        c.lifecycleHandle.domain()!=materialLifecycleDomain_ ||
        (s.removal && (c.runtime>=materialLifecycleHandles_.size() || materialLifecycleHandles_[c.runtime]!=c.lifecycleHandle)) ||
        (s.replay && !s.removal && materialTailVacancy_!=c.cookie)) return;
    if(s.removal && (materialEntryOwners_.empty() || materialEntryOwners_.back()!=c.cookie ||
        !GltfMaterialRecordsEqual(exportSource_.materials.back(),c.authored) ||
        !GltfMaterialRecordsEqual(gltfModel_.materials.back(),c.evaluated) || !s.audit || !s.referenceGraph ||
        !CanRemoveMaterialTail({exportSource_,gltfModel_,*s.referenceGraph,materialVariants_,
            {materialAssetIdentity(),sourceMaterialToRuntime_,uint32_t(materialCount()),uint32_t(primitiveRanges().size())},
            uint32_t(c.source),c.runtime,s.protectedMaterials},*s.audit,MaterialTailAuditPhase::AfterBindings))) return;
  }
  assert(s.descriptorsReady && s.expectedPlayer==player && s.epoch==generationEpoch_ && s.material==materialRevision_);
  using std::swap;
  static_assert(std::is_nothrow_move_assignable_v<tinygltf::Material>);
  static_assert(std::is_nothrow_swappable_v<AnimationPlayer>);
  const auto source=size_t(s.plan.destinationMaterial);
  // All material and resource guards precede either exchange. Descriptors and
  // converted material entries already name this same prospective generation.
  if(s.resources) {
    exchangeMaterialResources(*s.resources);
    if(*s.resources) return;
  } else {
    const bool published=ExchangeTextureResourceLifecycle(*s.dependencyChange,textureResourceLifecycle_,textureResourceLifecycleView());
    assert(published);(void)published;
  }
  // All catalog checks precede the first exchange. Registrations and phase
  // transitions are mutually exclusive and publish only preallocated rows.
  if(s.lifecycleRegistration) {const bool ok=materialLifecycle_->exchange(*s.lifecycleRegistration);assert(ok);(void)ok;}
  if(s.lifecycleTransition) {const bool ok=materialLifecycle_->exchange(*s.lifecycleTransition);assert(ok);(void)ok;}
  const auto exchangeRecord=[](tinygltf::Material& a,tinygltf::Material& b,tinygltf::Material& scratch) noexcept {
    scratch=std::move(a);a=std::move(b);b=std::move(scratch);
  };
  if(s.appended) {
    auto& tables=*s.appended;
    exportSource_.materials.swap(tables.authored);gltfModel_.materials.swap(tables.evaluated);
    exportMaterialSources_.swap(tables.exported);exportMaterialTextureBindings_.swap(tables.bindings);
    sourceMaterialToRuntime_.swap(tables.mapping);materialManager_.materials_.swap(tables.runtime.materials_);
    materialEntryOwners_.swap(tables.owners);materialTailVacancy_=s.futureVacancy;
    static_assert(std::is_nothrow_swappable_v<MaterialVariantLibrary>);
    assert(tables.variants);swap(materialVariants_,*tables.variants);
  } else if(s.retirement) {
    const auto& c=*s.retirement.state_;
    if(c.source>=0) {
      exchangeRecord(exportSource_.materials[size_t(c.source)],s.plan.authoredAfter,s.scratch[0]);
      exchangeRecord(gltfModel_.materials[size_t(c.source)],s.evaluated,s.scratch[1]);
    }
    if(s.exportRecord && c.runtime<exportMaterialSources_.size())
      exchangeRecord(*exportMaterialSources_[c.runtime],*s.exportRecord,s.scratch[2]);
    if(c.runtime<exportMaterialTextureBindings_.size()) swap(exportMaterialTextureBindings_[c.runtime],s.exportBindings);
    const bool updated=materialManager_.updateMaterial(c.runtime,s.converted.value);assert(updated);(void)updated;
  } else {
    exchangeRecord(exportSource_.materials[source],s.plan.authoredAfter,s.scratch[0]);
    exchangeRecord(gltfModel_.materials[source],s.evaluated,s.scratch[1]);
    exchangeRecord(*exportMaterialSources_[s.runtime],*s.exportRecord,s.scratch[2]);
    swap(exportMaterialTextureBindings_[s.runtime],s.exportBindings);
    const bool updated=materialManager_.updateMaterial(s.runtime,s.converted.value);assert(updated);(void)updated;
  }
  exportSource_.extensionsUsed.swap(s.authoredDeclarations);gltfModel_.extensionsUsed.swap(s.evaluatedDeclarations);
  runtimeMaterialProvenance_.swap(s.provenance);
  materialResourceDependencies_.swap(s.dependencies);
  gpuMaterials_.swap(s.gpu);gpuMaterialTemporalRevisions_.swap(s.temporal);swap(hasExtendedPbrMaterials_,s.extended);
  swap(materialBuffer_,s.buffer);swap(materialBufferCapacity_,s.capacity);
  swap(descriptorPool_,s.pool);swap(ownsDescriptorPool_,s.ownsPool);
  descriptorSets_.swap(s.primary);auxiliaryDescriptorSets_.swap(s.auxiliary);
  if(player && s.player) swap(*player,*s.player);
  materialLifecycleHandles_.swap(s.lifecycleHandles);materialLifecycleNextSlot_=s.lifecycleNextAfter;
  s.lifecyclePreparation.reset();
  if(s.command) {
    auto& c=*s.command.state_;
    if(s.removal) {c.childVacancy=s.expectedVacancy;c.phase=MaterialAppendCommand::State::Phase::Detached;}
    else c.phase=MaterialAppendCommand::State::Phase::Present;
  }
  if(s.retirement) {
    auto& c=*s.retirement.state_;c.authority=s.lifecycleTransition->command();c.published=true;
    c.retired=s.retirementTarget==MaterialRetirementState::Retired;
  }
  // The live generation now owns the prospective complete set. This consumed
  // token retains only the complete image set used by its exchanged old pool;
  // dropping its extra prospective pointer cannot release a last image owner.
  s.images=s.beforeImages;
  ++generationEpoch_;++materialRevision_;++authoredMaterialRevision_;s.consumed=true;
}
} // namespace container::scene
