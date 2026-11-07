#include <Container/utility/SceneManager.h>
#include <Container/geometry/GltfTextureResourceReferences.h>
#include <Container/geometry/GltfAnimation.h>

#include <algorithm>
#include <cassert>
#include <map>
#include <set>
#include <tuple>
#include <stdexcept>
#include <utility>

namespace container::scene {
namespace {
using namespace container::util;
using namespace container::geometry::gltf;
[[noreturn]] void sourceFailure(const char* reason) {throw std::invalid_argument(reason);}
struct SourceBudget {
  uint64_t remaining;
  uint64_t work{32'000'000};
  void add(uint64_t count,uint64_t width=1) {
    if(width && count>remaining/width) throw std::length_error("Source resource retirement exceeds its CPU budget");
    remaining-=count*width;
  }
  void visit(uint64_t count=1) {
    if(count>work) throw std::length_error("Resource retirement reference planning exceeds its work budget");work-=count;
  }
};
struct SelectionLess {
  bool operator()(const TextureResourceSelection& a,const TextureResourceSelection& b) const noexcept {
    return std::tie(a.domain,a.kind,a.row)<std::tie(b.domain,b.kind,b.row);
  }
};
template<class Request> bool sameRequest(const Request& a,const Request& b) noexcept {
  return a.active==b.active && a.key==b.key && a.encoded==b.encoded && a.archive==b.archive && a.evaluated==b.evaluated &&
      a.sourceIdentity==b.sourceIdentity && a.inputSourceIdentity==b.inputSourceIdentity;
}
}

struct SceneManager::SourceResourceRetirementCommand::State {
  const SceneManager* owner{};
  std::shared_ptr<const uint8_t> ownerIdentity,resourceOwner;
  TextureResourceRetirementCommand authority;
  container::material::RuntimeTextureRetirementCommand runtimeAuthority;
  container::material::TextureImageOwnershipPtr images;
  std::vector<TextureResourceSelection> selections;
  struct TextureDelta {TextureResourceSelection selection;uint32_t color{},data{};int32_t evaluated{-1};bool colorChanged{},dataChanged{},mappingChanged{};};
  struct ExportDelta {uint32_t runtime{};int32_t texture{-1};};
  struct CacheDelta {size_t index{};MaterialTextureSourceRequest original;};
  std::vector<TextureDelta> textures;
  std::vector<ExportDelta> exports;
  std::vector<CacheDelta> cache;
  uint64_t bytes{},version{};
  bool published{},retired{};
};

struct SceneManager::PreparedSourceResourceRetirement::State {
  const SceneManager* owner{};
  std::shared_ptr<const uint8_t> ownerIdentity,resourceOwner;
  const AnimationPlayer* player{};
  uint64_t epoch{},geometry{},material{},authored{},resource{},commandVersion{};
  SourceResourceRetirementCommand receipt;
  bool retiring{},initial{},consumed{},cpuOnly{};
  SourceResourcePhaseTable beforeArchive,beforeEvaluated,archive,evaluated;
  container::material::GltfTextureResources beforeArchiveRoles,beforeEvaluatedRoles,archiveRoles,evaluatedRoles;
  std::vector<int32_t> beforeCorrespondence,correspondence,beforeExport,exportMapping;
  std::vector<uint8_t> materialMask;
  size_t requestCount{};
  struct CacheSwap {size_t index{};MaterialTextureSourceRequest expected,replacement;};
  std::vector<CacheSwap> cache;
  TextureResourceLifecycleGenerationPtr before;
  PreparedTextureResourceLifecycle mutation;
  std::optional<container::material::PreparedRuntimeTextureRetirement> runtimeMutation;
  std::vector<uint32_t> runtimeSlots;
  std::shared_ptr<PreparedTextureDescriptorState> descriptors;
  std::vector<VkSampler> liveDescriptorSamplers;
  std::array<std::weak_ptr<const uint8_t>,16> descriptorFrames;
  TextureResourceLifecycleGenerationPtr futureGeneration;
  container::material::TextureImageOwnershipPtr futureImages;
  std::vector<MaterialResourceDependencyStatePtr> beforeDependencies,dependencies;
  container::material::TextureImageOwnershipPtr images;
  TextureResourceReferenceOwnerPtr beforeReaders,futurePreparation;
  GltfTextureResourceReferences beforeArchiveProof,beforeEvaluatedProof,archiveProof,evaluatedProof;
  std::vector<GltfMaterialRawTextureReferences> restProofs;
  TextureResourceLifecycleView view() const noexcept {
    auto result=owner->textureResourceLifecycleView();
    result.archive.colorResources=archiveRoles.color;result.archive.dataResources=archiveRoles.data;
    result.archive.imageActive=archive.images;result.archive.textureActive=archive.textures;result.archive.samplerActive=archive.samplers;
    result.evaluated.colorResources=evaluatedRoles.color;result.evaluated.dataResources=evaluatedRoles.data;
    result.evaluated.imageActive=evaluated.images;result.evaluated.textureActive=evaluated.textures;result.evaluated.samplerActive=evaluated.samplers;
    return {result.archive,result.evaluated,runtimeMutation?runtimeMutation->textures():owner->textureManager_,
        correspondence,result.archiveTextureIdentities};
  }
  static GltfTextureResourceMasks masks(const SourceResourcePhaseTable& phases,std::span<const uint8_t> materials) noexcept {
    return {materials,phases.textures,phases.images,phases.samplers};
  }
  bool matches(const SceneManager& current,const AnimationPlayer* currentPlayer) const noexcept {
    // Compare the caller's borrowed pointer before ever reading its contents.
    if(consumed || owner!=&current || player!=currentPlayer || ownerIdentity!=current.sceneRootOwnerIdentity_ ||
        resourceOwner!=current.materialResourceOwnerIdentity_ || !receipt || !receipt.state_ ||
        receipt.state_->owner!=&current || receipt.state_->ownerIdentity!=ownerIdentity || receipt.state_->resourceOwner!=resourceOwner ||
        receipt.state_->version!=commandVersion || receipt.state_->published==initial ||
        (!initial && receipt.state_->retired==retiring) || current.materialResourceRevision_!=resource ||
        current.generationEpoch_!=epoch || current.geometryRevision_!=geometry || current.materialRevision_!=material ||
        current.authoredMaterialRevision_!=authored || current.textureResourceLifecycle_!=before ||
        current.materialResourceDependencies_!=beforeDependencies || !before || !before->matches(current.textureResourceLifecycleView()) ||
        !current.materialResourceDependenciesValid(currentPlayer) || !current.textureSamplingStateValidForResources() ||
        current.textureImageOwnership_!=images || !current.allocationManager_ ||
        !container::material::TextureImageOwnershipMatches(images,current.textureManager_,*current.allocationManager_) ||
        current.archiveResourcePhases_!=beforeArchive || current.gltfResourcePhases_!=beforeEvaluated ||
        current.archiveTextureResources_.color!=beforeArchiveRoles.color || current.archiveTextureResources_.data!=beforeArchiveRoles.data ||
        current.gltfTextureResources_.color!=beforeEvaluatedRoles.color || current.gltfTextureResources_.data!=beforeEvaluatedRoles.data ||
        current.archiveTextureToEvaluated_!=beforeCorrespondence || current.exportTextureIndices_!=beforeExport ||
        current.materialTextureSourceRequests_.size()!=requestCount || !beforeReaders || !beforeReaders->valid() ||
        !futurePreparation || !futurePreparation->valid()) return false;
    if(retiring && before->opaqueReferenceOwnerCount()) return false;
    if(runtimeMutation && (!container::material::CanExchangeRuntimeTextureRetirement(*runtimeMutation,current.textureManager_) ||
        (!cpuOnly && (!descriptors || !current.textureDescriptorStateMatches(*descriptors,runtimeMutation->textures()))) ||
        !container::material::TextureImageOwnershipMatches(futureImages,runtimeMutation->textures(),*current.allocationManager_) ||
        !container::material::TextureImageOwnershipValid(receipt.state_->images,*current.allocationManager_))) return false;
    if(materialMask.size()!=current.sourceMaterialToRuntime_.size()) return false;
    for(size_t source=0;source<materialMask.size();++source)
      if(bool(materialMask[source])!=current.materialIsActive(current.sourceMaterialToRuntime_[source])) return false;
    for(const auto& delta:cache) if(delta.index>=current.materialTextureSourceRequests_.size() ||
        !sameRequest(current.materialTextureSourceRequests_[delta.index],delta.expected)) return false;
    if(!GltfTextureResourceReferencesMatch(current.exportSource_,masks(beforeArchive,materialMask),beforeArchiveProof) ||
        !GltfTextureResourceReferencesMatch(current.gltfModel_,masks(beforeEvaluated,materialMask),beforeEvaluatedProof) ||
        !GltfTextureResourceReferencesMatch(current.exportSource_,masks(archive,materialMask),archiveProof) ||
        !GltfTextureResourceReferencesMatch(current.gltfModel_,masks(evaluated,materialMask),evaluatedProof)) return false;
    const auto next=mutation.next();if(!next) return false;
    for(uint32_t runtime=0;runtime<current.materialCount();++runtime) if(current.materialIsActive(runtime)) {
      const auto live=current.materialResourceLiveOwner(runtime);
      if(!live || !live->active() || !live->matches(next) || (retiring && !live->complete())) return false;
    }
    if(currentPlayer) {
      if(restProofs.size()!=materialMask.size() || currentPlayer->library().restMaterials.size()<restProofs.size()) return false;
      for(size_t source=0;source<restProofs.size();++source) if(materialMask[source]) {
        if(!GltfMaterialRawTextureReferencesMatch(currentPlayer->library().restMaterials[source],
            next->rowCount(ResourceDomainKind::Archive,ResourceKind::Texture),{},restProofs[source])) return false;
        for(const auto texture:restProofs[source].textures())
          if(!next->active({ResourceDomainKind::Archive,ResourceKind::Texture,texture})) return false;
      }
    } else if(!restProofs.empty()) return false;
    return current.materialResourceDependencyMapMatches(dependencies,next,currentPlayer,true,std::span<const int32_t>(exportMapping)) &&
        current.materialResourceDependencyMapWillBeActive(dependencies,mutation) &&
        CanExchangeTextureResourceLifecycle(mutation,before,view());
  }
};

SceneManager::SourceResourceRetirementCommand::operator bool() const noexcept {return bool(state_);}
bool SceneManager::SourceResourceRetirementCommand::published() const noexcept {return state_ && state_->published;}
bool SceneManager::SourceResourceRetirementCommand::retired() const noexcept {return state_ && state_->published && state_->retired;}
uint64_t SceneManager::SourceResourceRetirementCommand::ownedBytes() const noexcept {return state_?state_->bytes:0;}
std::span<const TextureResourceSelection> SceneManager::SourceResourceRetirementCommand::selections() const noexcept {
  return state_?std::span<const TextureResourceSelection>(state_->selections):std::span<const TextureResourceSelection>{};
}
TextureResourceReferenceOwnerPtr SceneManager::SourceResourceRetirementCommand::references() const noexcept {
  return state_?state_->authority.references():nullptr;
}
container::material::TextureImageOwnershipPtr SceneManager::SourceResourceRetirementCommand::textureImageOwnership() const noexcept {
  return state_?state_->images:nullptr;
}
SceneManager::PreparedSourceResourceRetirement::PreparedSourceResourceRetirement() noexcept=default;
SceneManager::PreparedSourceResourceRetirement::~PreparedSourceResourceRetirement()=default;
SceneManager::PreparedSourceResourceRetirement::PreparedSourceResourceRetirement(PreparedSourceResourceRetirement&&) noexcept=default;
SceneManager::PreparedSourceResourceRetirement& SceneManager::PreparedSourceResourceRetirement::operator=(PreparedSourceResourceRetirement&&) noexcept=default;
SceneManager::PreparedSourceResourceRetirement::operator bool() const noexcept {return state_ && !state_->consumed;}
SceneManager::SourceResourceRetirementCommand SceneManager::PreparedSourceResourceRetirement::command() const noexcept {
  return state_?state_->receipt:SourceResourceRetirementCommand{};
}
bool SceneManager::PreparedSourceResourceRetirement::runtimeChanged() const noexcept {return state_ && bool(state_->runtimeMutation);}
size_t SceneManager::PreparedSourceResourceRetirement::textureResourceCount() const noexcept {
  return *this?(state_->runtimeMutation?state_->runtimeMutation->textures().textureCount():state_->owner->textureManager_.textureCount()):0;
}
const container::material::TextureManager& SceneManager::PreparedSourceResourceRetirement::textureResources() const {
  if(!*this) sourceFailure("Prepared runtime resource table is unavailable");
  return state_->runtimeMutation?state_->runtimeMutation->textures():state_->owner->textureManager_;
}
bool SceneManager::PreparedSourceResourceRetirement::textureActive(uint32_t row) const noexcept {
  return *this && (state_->runtimeMutation?state_->runtimeMutation->textures():state_->owner->textureManager_).isTextureActive(row);
}
container::material::TextureImageOwnershipPtr SceneManager::PreparedSourceResourceRetirement::textureImageOwnership() const noexcept {
  return state_?state_->futureImages:nullptr;
}
TextureResourceLifecycleGenerationPtr SceneManager::PreparedSourceResourceRetirement::resourceLifecycleGeneration() const noexcept {
  return state_?state_->futureGeneration:nullptr;
}
const container::gpu::AllocatedBuffer& SceneManager::PreparedSourceResourceRetirement::metadataBuffer() const {
  if(!*this) sourceFailure("Prepared resource descriptors are unavailable");
  return state_->descriptors?state_->descriptors->metadataBuffer:state_->owner->textureMetadataBuffer_;
}
std::span<const VkDescriptorImageInfo> SceneManager::PreparedSourceResourceRetirement::imageInfos() const noexcept {
  if(!*this) return {};return state_->descriptors?std::span<const VkDescriptorImageInfo>(state_->descriptors->images):
      std::span<const VkDescriptorImageInfo>(state_->owner->textureSamplingPlan_.images);
}
std::span<const VkSampler> SceneManager::PreparedSourceResourceRetirement::descriptorSamplers() const noexcept {
  if(!*this) return {};
  return state_->descriptors?std::span<const VkSampler>(state_->descriptors->descriptorSamplers):std::span<const VkSampler>(state_->liveDescriptorSamplers);
}
const container::material::TextureSamplingPlan& SceneManager::PreparedSourceResourceRetirement::samplingPlan() const {
  if(!*this) sourceFailure("Prepared resource sampling plan is unavailable");
  return state_->descriptors?state_->descriptors->sampling:state_->owner->textureSamplingPlan_;
}
TextureResourceReferenceOwnerPtr SceneManager::PreparedSourceResourceRetirement::retainAllResourceReferences(ResourcePinClass pinClass,uint64_t maxBytes) const {
  if(!*this) sourceFailure("Prepared resource reference generation is unavailable");
  if(!maxBytes || maxBytes==UINT64_MAX) sourceFailure("Prepared resource references require a finite CPU budget");
  if(pinClass==ResourcePinClass::Preparation) {
    if(!state_->futurePreparation || state_->futurePreparation->ownedBytes()>maxBytes)
      throw std::length_error("Prepared resource reference retention exceeds its CPU budget");
    return state_->futurePreparation;
  }
  return state_->mutation.selectAll(pinClass,maxBytes);
}
void SceneManager::PreparedSourceResourceRetirement::writeDescriptorSetContents(VkDescriptorSet set,
    const container::gpu::AllocatedBuffer& camera,const container::gpu::AllocatedBuffer& objects,
    const container::gpu::AllocatedBuffer& materials,size_t materialCount,const AnimationPlayer* currentPlayer) const {
  if(!*this || !publicationMatches(*state_->owner,false,currentPlayer)) sourceFailure("Prepared resource descriptors are stale");
  if(state_->descriptors) state_->owner->writeTextureDescriptorSetContents(set,camera,objects,materials,materialCount,*state_->descriptors);
  else state_->owner->writeMaterialDescriptorSetContents(set,camera,objects,materials,materialCount);
}
bool SceneManager::PreparedSourceResourceRetirement::publicationMatches(const SceneManager& current,bool consumed,const AnimationPlayer* currentPlayer) const noexcept {
  if(!state_) return false;const auto& s=*state_;
  if(s.player!=currentPlayer) return false;
  if(!consumed) return s.matches(current,currentPlayer);
  return s.consumed && s.owner==&current && s.ownerIdentity==current.sceneRootOwnerIdentity_ &&
      s.resourceOwner==current.materialResourceOwnerIdentity_ && s.resource!=UINT64_MAX && current.materialResourceRevision_==s.resource+1 &&
      current.geometryRevision_==s.geometry && current.materialRevision_==s.material && current.authoredMaterialRevision_==s.authored &&
      current.textureResourceLifecycle_==s.futureGeneration && current.textureImageOwnership_==s.futureImages &&
      current.textureSamplingStateValid() && current.materialResourceDependenciesValid(currentPlayer) &&
      current.textureResourceLifecycle_->matches(current.textureResourceLifecycleView()) &&
      container::material::TextureImageOwnershipMatches(s.futureImages,current.textureManager_,*current.allocationManager_);
}
bool SceneManager::PreparedSourceResourceRetirement::markDescriptorFramesReady(const std::shared_ptr<const uint8_t>& witness) const noexcept {
  if(!state_ || state_->consumed || !witness) return false;
  for(auto& slot:state_->descriptorFrames) if(slot.expired()) {slot=witness;return true;}
  return false;
}

std::vector<TextureResourceSelection> SceneManager::planResourceRetirement(TextureResourceSelection requested,
    const AnimationPlayer* player,uint64_t maxBytes) const {
  if(!maxBytes || maxBytes==UINT64_MAX) sourceFailure("Resource retirement planning requires a finite CPU budget");
  SourceBudget budget{maxBytes};refreshTextureResourceLifecycle(ResourceRetirementPolicy::BlockPreparations,budget.remaining,player);
  const auto generation=resourceLifecycleGeneration();
  if(!generation || !generation->active(requested) || !materialResourceDependenciesValid(player) ||
      !generation->matches(textureResourceLifecycleView())) sourceFailure("The selected resource incarnation is unavailable or retired");
  if((requested.domain==ResourceDomainKind::Runtime && requested.kind!=ResourceKind::RuntimeTexture) ||
      (requested.domain!=ResourceDomainKind::Runtime && requested.kind!=ResourceKind::Image &&
       requested.kind!=ResourceKind::Texture && requested.kind!=ResourceKind::Sampler)) sourceFailure("The selected resource kind has no retirement plan");
  if(generation->opaqueReferenceOwnerCount()) sourceFailure("Unknown retained resource metadata prevents a complete unused-resource proof");
  if(exportSource_.materials.size()!=sourceMaterialToRuntime_.size() || gltfModel_.materials.size()!=sourceMaterialToRuntime_.size())
    sourceFailure("Resource planning has no exact source material activity map");
  budget.add(sourceMaterialToRuntime_.size());std::vector<uint8_t> materialMask;materialMask.reserve(sourceMaterialToRuntime_.size());
  for(const auto runtime:sourceMaterialToRuntime_) {budget.visit();materialMask.push_back(materialIsActive(runtime)?1:0);}
  GltfTextureResourceReferenceLimits proofLimits;proofLimits.maxOwnedBytes=std::min(proofLimits.maxOwnedBytes,budget.remaining);
  const auto archive=ExtractGltfTextureResourceReferences(exportSource_,PreparedSourceResourceRetirement::State::masks(archiveResourcePhases_,materialMask),proofLimits);
  budget.add(archive.stats().ownedBytes);proofLimits.maxOwnedBytes=std::min(proofLimits.maxOwnedBytes,budget.remaining);
  const auto evaluated=ExtractGltfTextureResourceReferences(gltfModel_,PreparedSourceResourceRetirement::State::masks(gltfResourcePhases_,materialMask),proofLimits);
  budget.add(evaluated.stats().ownedBytes);
  using HandleKey=std::tuple<uint64_t,ResourceKind,uint32_t,uint64_t>;
  const auto key=[](const ResourceHandle& handle) {return HandleKey{handle.domain().identity(),handle.kind(),handle.slot(),handle.incarnation()};};
  std::set<HandleKey> materialUses;
  for(uint32_t runtime=0;runtime<materialCount();++runtime) if(materialIsActive(runtime)) {
    const auto owner=materialResourceLiveOwner(runtime);if(!owner || !owner->active() || !owner->complete())
      sourceFailure("An active material has incomplete exact resource dependencies");
    for(const auto& handle:owner->resources()) {budget.visit();const auto value=key(handle);
      if(!materialUses.contains(value)) {budget.add(96);materialUses.insert(value);}}
  }
  const auto materialUse=[&](TextureResourceSelection selection) {return materialUses.contains(key(generation->resource(selection)));};
  std::set<TextureResourceSelection,SelectionLess> planned,neighbours;
  const auto add=[&](auto& rows,TextureResourceSelection selection) {
    if(!generation->active(selection) || rows.contains(selection)) return;
    if(rows.size()>=4096) sourceFailure("Resource orphan closure exceeds its bounded selected capacity");
    budget.add(96);rows.insert(selection);
  };
  add(planned,requested);
  // A/E correspondence is explicit authority. A shared evaluated identity is
  // preserved when deleting only one archive identity. Direct evaluated
  // deletion includes every archive correspondence that would become invalid.
  if(requested.kind==ResourceKind::Texture && requested.domain==ResourceDomainKind::Archive) {
    if(requested.row>=archiveTextureToEvaluated_.size()) sourceFailure("Archive texture correspondence is unavailable");
    const auto target=archiveTextureToEvaluated_[requested.row];bool shared=false;
    for(uint32_t row=0;row<archiveTextureToEvaluated_.size();++row) {budget.visit();
      if(row!=requested.row && generation->active({ResourceDomainKind::Archive,ResourceKind::Texture,row}) &&
          archiveTextureToEvaluated_[row]==target) shared=true;}
    if(target>=0 && !shared && !materialUse({ResourceDomainKind::Evaluated,ResourceKind::Texture,uint32_t(target)}))
      add(planned,{ResourceDomainKind::Evaluated,ResourceKind::Texture,uint32_t(target)});
  } else if(requested.kind==ResourceKind::Texture && requested.domain==ResourceDomainKind::Evaluated) {
    for(uint32_t row=0;row<archiveTextureToEvaluated_.size();++row) {budget.visit();
      if(archiveTextureToEvaluated_[row]==int32_t(requested.row)) add(planned,{ResourceDomainKind::Archive,ResourceKind::Texture,row});}
  }
  for(const auto selection:planned) {
    if(materialUse(selection)) sourceFailure("An active material directly references the selected resource");
    if(selection.kind!=ResourceKind::Texture) continue;
    const auto& proof=selection.domain==ResourceDomainKind::Archive?archive:evaluated;
    for(const auto image:proof.textureImages(selection.row)) add(neighbours,{selection.domain,ResourceKind::Image,image});
    for(const auto sampler:proof.textureSamplers(selection.row)) add(neighbours,{selection.domain,ResourceKind::Sampler,sampler});
    const auto& roles=selection.domain==ResourceDomainKind::Archive?archiveTextureResources_:gltfTextureResources_;
    if(selection.row>=roles.color.size() || selection.row>=roles.data.size()) sourceFailure("Selected source texture roles are unavailable");
    for(const auto runtime:{roles.color[selection.row],roles.data[selection.row]}) if(runtime!=UINT32_MAX)
      add(neighbours,{ResourceDomainKind::Runtime,ResourceKind::RuntimeTexture,runtime});
  }
  // Every other active Texture preserves its actual image alternatives,
  // sampler, and both runtime roles, even when it is not drawn by a material.
  for(const auto domain:{ResourceDomainKind::Archive,ResourceDomainKind::Evaluated}) {
    const auto& proof=domain==ResourceDomainKind::Archive?archive:evaluated;
    const auto& roles=domain==ResourceDomainKind::Archive?archiveTextureResources_:gltfTextureResources_;
    for(uint32_t row=0;row<proof.textureCount();++row) {
      budget.visit();if(!proof.textureActive(row) || planned.contains({domain,ResourceKind::Texture,row})) continue;
      for(const auto image:proof.textureImages(row)) {budget.visit();neighbours.erase({domain,ResourceKind::Image,image});}
      for(const auto sampler:proof.textureSamplers(row)) {budget.visit();neighbours.erase({domain,ResourceKind::Sampler,sampler});}
      if(row>=roles.color.size() || row>=roles.data.size()) sourceFailure("Surviving source role coverage is unavailable");
      for(const auto runtime:{roles.color[row],roles.data[row]}) if(runtime!=UINT32_MAX)
        neighbours.erase({ResourceDomainKind::Runtime,ResourceKind::RuntimeTexture,runtime});
    }
    for(const auto reference:proof.sourceReferences()) {
      budget.visit();const auto kind=reference.kind==GltfTextureResourceReferenceKind::Image?ResourceKind::Image:
          reference.kind==GltfTextureResourceReferenceKind::Texture?ResourceKind::Texture:ResourceKind::Sampler;
      const TextureResourceSelection selection{domain,kind,reference.index};
      if(planned.contains(selection)) sourceFailure("A raw source document references the selected resource");
      neighbours.erase(selection);
    }
  }
  for(const auto neighbour:neighbours) if(!materialUse(neighbour)) add(planned,neighbour);
  budget.add(planned.size(),sizeof(TextureResourceSelection));std::vector<TextureResourceSelection> result(planned.begin(),planned.end());
  // The same sealed combined transaction proves all residual exact live pins,
  // caches, rest records and source schemas. Preview allocates no GPU buffer,
  // publishes nothing, and retains no parent catalog after returning.
  auto proof=prepareSourceResourceRetirementImpl(result,nullptr,ResourceLifecyclePhase::Retired,player,budget.remaining,false);
  if(!proof.state_->matches(*this,player)) sourceFailure("Resource orphan closure became stale during its proof");
  return result;
}

SceneManager::PreparedSourceResourceRetirement SceneManager::prepareSourceResourceRetirement(
    std::span<const TextureResourceSelection> selections,const AnimationPlayer* player,uint64_t maxBytes) const {
  return prepareSourceResourceRetirementImpl(selections,nullptr,ResourceLifecyclePhase::Retired,player,maxBytes);
}
SceneManager::PreparedSourceResourceRetirement SceneManager::prepareSourceResourceRetirement(
    const SourceResourceRetirementCommand& command,ResourceLifecyclePhase target,const AnimationPlayer* player,uint64_t maxBytes) const {
  return prepareSourceResourceRetirementImpl({},&command,target,player,maxBytes);
}

SceneManager::PreparedSourceResourceRetirement SceneManager::prepareSourceResourceRetirementImpl(
    std::span<const TextureResourceSelection> selections,const SourceResourceRetirementCommand* replay,
    ResourceLifecyclePhase target,const AnimationPlayer* player,uint64_t maxBytes,bool prepareGpu) const {
  if(!maxBytes || maxBytes==UINT64_MAX || (target!=ResourceLifecyclePhase::Active && target!=ResourceLifecyclePhase::Retired) ||
      (!replay && (selections.empty() || selections.size()>4096))) sourceFailure("Source retirement requires finite capacities and explicit source selections");
  SourceBudget budget{maxBytes};budget.add(sizeof(PreparedSourceResourceRetirement::State)+256);
  refreshTextureResourceLifecycle(ResourceRetirementPolicy::BlockPreparations,budget.remaining,player);
  if(materialResourceRevision_==UINT64_MAX || !textureResourceLifecycle_ || !materialResourceDependenciesValid(player) ||
      !textureSamplingStateValidForResources() || !allocationManager_ ||
      !container::material::TextureImageOwnershipMatches(textureImageOwnership_,textureManager_,*allocationManager_))
    sourceFailure("Source retirement requires current material, sampling and actual image ownership");
  const bool retiring=target==ResourceLifecyclePhase::Retired;
  if(retiring && textureResourceLifecycle_->opaqueReferenceOwnerCount()) sourceFailure("Opaque retained resource references prevent source retirement");
  PreparedSourceResourceRetirement result;result.state_=std::make_shared<PreparedSourceResourceRetirement::State>();auto& s=*result.state_;
  s.owner=this;s.ownerIdentity=sceneRootOwnerIdentity_;s.resourceOwner=materialResourceOwnerIdentity_;s.player=player;
  s.epoch=generationEpoch_;s.geometry=geometryRevision_;s.material=materialRevision_;s.authored=authoredMaterialRevision_;s.resource=materialResourceRevision_;
  s.retiring=retiring;s.initial=!replay;s.cpuOnly=!prepareGpu;s.before=textureResourceLifecycle_;s.images=textureImageOwnership_;
  budget.add(materialSamplerDescriptorCapacity_,sizeof(VkSampler));s.liveDescriptorSamplers.reserve(materialSamplerDescriptorCapacity_);
  for(uint32_t slot=0;slot<materialSamplerDescriptorCapacity_;++slot)
    s.liveDescriptorSamplers.push_back(slot<materialSamplerSlots_.keys.size()?materialSamplers_[materialSamplerSlots_.keys[slot].value]:baseColorSampler_);
  if(replay) {
    if(!*replay || replay->state_->owner!=this || replay->state_->ownerIdentity!=sceneRootOwnerIdentity_ ||
        replay->state_->resourceOwner!=materialResourceOwnerIdentity_ || !replay->state_->published ||
        replay->state_->retired==retiring || replay->state_->version==UINT64_MAX)
      sourceFailure("Source resource replay requires its original published command and opposite phase");
    s.receipt=*replay;
  } else {
    budget.add(sizeof(SourceResourceRetirementCommand::State)+256);
    s.receipt.state_=std::make_shared<SourceResourceRetirementCommand::State>();auto& command=*s.receipt.state_;
    command.owner=this;command.ownerIdentity=sceneRootOwnerIdentity_;command.resourceOwner=materialResourceOwnerIdentity_;
    budget.add(selections.size(),sizeof(TextureResourceSelection));command.selections.assign(selections.begin(),selections.end());
  }
  auto& command=*s.receipt.state_;s.commandVersion=command.version;
  const auto selected=std::span<const TextureResourceSelection>(command.selections);
  budget.add(selected.size(),96);std::set<std::pair<ResourceDomainKind,uint32_t>> selectedTextures;
  for(const auto selection:selected) if(selection.kind==ResourceKind::Texture)
    selectedTextures.emplace(selection.domain,selection.row);
  budget.add(selected.size(),sizeof(uint32_t));
  for(const auto selection:selected) if(selection.domain==ResourceDomainKind::Runtime && selection.kind==ResourceKind::RuntimeTexture)
    s.runtimeSlots.push_back(selection.row);
  std::ranges::sort(s.runtimeSlots);
  if(!s.runtimeSlots.empty()) {
    container::material::RuntimeTextureRetirementLimits runtimeLimits;runtimeLimits.maxOwnedBytes=budget.remaining;
    s.runtimeMutation.emplace(replay?container::material::PrepareRuntimeTextureRetirementReplay(textureManager_,command.runtimeAuthority,
        retiring?container::material::RuntimeTextureRetirementTarget::Retired:container::material::RuntimeTextureRetirementTarget::Active,runtimeLimits):
        container::material::PrepareRuntimeTextureRetirement(textureManager_,s.runtimeSlots,runtimeLimits));
    budget.add(s.runtimeMutation->ownedBytes());
    if(!replay) {
      command.runtimeAuthority=s.runtimeMutation->command();
      command.images=container::material::SelectTextureImageOwnership(s.images,s.runtimeSlots,*allocationManager_,budget.remaining);
      budget.add(command.images->ownedBytes());
    } else if(!container::material::TextureImageOwnershipValid(command.images,*allocationManager_))
      sourceFailure("Runtime resource history has lost its selected actual image ownership");
    const std::array inherited{s.images,command.images};
    s.futureImages=container::material::PrepareTextureImageOwnership(s.runtimeMutation->textures(),*allocationManager_,inherited,budget.remaining);
    budget.add(s.futureImages->ownedBytes());
  } else s.futureImages=s.images;
  // Full vector storage belongs only to this preparation. History stores the
  // selected exact deltas and helper receipt, never a full parent catalog.
  const auto phaseBytes=exportSource_.images.size()+exportSource_.textures.size()+exportSource_.samplers.size()+
      gltfModel_.images.size()+gltfModel_.textures.size()+gltfModel_.samplers.size();
  budget.add(phaseBytes,2);s.beforeArchive=archiveResourcePhases_;s.beforeEvaluated=gltfResourcePhases_;
  s.archive=s.beforeArchive;s.evaluated=s.beforeEvaluated;
  budget.add(archiveTextureResources_.color.size()+archiveTextureResources_.data.size()+
      gltfTextureResources_.color.size()+gltfTextureResources_.data.size(),2*sizeof(uint32_t));
  s.beforeArchiveRoles=s.archiveRoles=archiveTextureResources_;s.beforeEvaluatedRoles=s.evaluatedRoles=gltfTextureResources_;
  budget.add(archiveTextureToEvaluated_.size()+exportTextureIndices_.size(),2*sizeof(int32_t));
  s.beforeCorrespondence=s.correspondence=archiveTextureToEvaluated_;s.beforeExport=s.exportMapping=exportTextureIndices_;
  const auto changePhase=[&](TextureResourceSelection selection) {
    if(selection.domain==ResourceDomainKind::Runtime && selection.kind==ResourceKind::RuntimeTexture) return;
    if((selection.domain!=ResourceDomainKind::Archive && selection.domain!=ResourceDomainKind::Evaluated) ||
        (selection.kind!=ResourceKind::Image && selection.kind!=ResourceKind::Texture && selection.kind!=ResourceKind::Sampler))
      sourceFailure("Source retirement does not retire runtime or material rows");
    auto& phases=selection.domain==ResourceDomainKind::Archive?s.archive:s.evaluated;
    const auto& source=selection.domain==ResourceDomainKind::Archive?exportSource_:gltfModel_;
    auto* mask=selection.kind==ResourceKind::Image?&phases.images:selection.kind==ResourceKind::Texture?&phases.textures:&phases.samplers;
    const auto count=selection.kind==ResourceKind::Image?source.images.size():selection.kind==ResourceKind::Texture?source.textures.size():source.samplers.size();
    if(selection.row>=count || sourceResourceActive(selection)!=retiring) sourceFailure("Source retirement selection has a stale phase");
    if(mask->empty()) mask->assign(count,1);if(mask->size()!=count) sourceFailure("Source phase table has an invalid shape");
    (*mask)[selection.row]=retiring?0:1;
  };
  for(const auto selection:selected) changePhase(selection);
  if(!replay) for(const auto domain:{ResourceDomainKind::Archive,ResourceDomainKind::Evaluated}) {
    const auto& roles=domain==ResourceDomainKind::Archive?archiveTextureResources_:gltfTextureResources_;
    for(uint32_t row=0;row<roles.color.size();++row) {
      if(row>=roles.data.size()) sourceFailure("Source texture role table is unavailable");
      budget.visit();const bool phase=selectedTextures.contains({domain,row});
      const bool color=phase || std::binary_search(s.runtimeSlots.begin(),s.runtimeSlots.end(),roles.color[row]);
      const bool data=phase || std::binary_search(s.runtimeSlots.begin(),s.runtimeSlots.end(),roles.data[row]);
      if(!color && !data) continue;
      if(command.textures.size()==command.textures.capacity()) {
        const auto capacity=std::max<size_t>(8,command.textures.size()*2);
        budget.add(capacity-command.textures.capacity(),sizeof(SourceResourceRetirementCommand::State::TextureDelta));
        command.textures.reserve(capacity);
      }
      command.textures.push_back({{domain,ResourceKind::Texture,row},roles.color[row],roles.data[row],
          domain==ResourceDomainKind::Archive?archiveTextureToEvaluated_.at(row):-1,color,data,phase && domain==ResourceDomainKind::Archive});
    }
  }
  for(const auto& delta:command.textures) {
    auto& roles=delta.selection.domain==ResourceDomainKind::Archive?s.archiveRoles:s.evaluatedRoles;
    const auto row=delta.selection.row;
    if(row>=roles.color.size() || row>=roles.data.size() || (delta.colorChanged && roles.color[row]!=(retiring?delta.color:UINT32_MAX)) ||
        (delta.dataChanged && roles.data[row]!=(retiring?delta.data:UINT32_MAX))) sourceFailure("Source replay lost its exact selected runtime roles");
    if(delta.colorChanged) roles.color[row]=retiring?UINT32_MAX:delta.color;
    if(delta.dataChanged) roles.data[row]=retiring?UINT32_MAX:delta.data;
    if(delta.mappingChanged) {
      if(row>=s.correspondence.size() || s.correspondence[row]!=(retiring?delta.evaluated:-1)) sourceFailure("Source replay lost its selected A/E correspondence");
      s.correspondence[row]=retiring?-1:delta.evaluated;
    }
  }
  TextureResourceLifecycleLimits limits;limits.maxOwnedBytes=budget.remaining;
  // Coexisting candidates retain the generic transition's own Preparation
  // pins. Only a successful publication invalidates their revision witnesses;
  // abandoning a newer candidate must leave an earlier one ready. Exact live
  // dependencies are still checked by the source/material proofs and catalog.
  limits.retirementPolicy=ResourceRetirementPolicy::InvalidatePreparations;
  s.mutation=replay?PrepareTextureResourceRetirementReplay(s.before,s.view(),command.authority,target,limits):
      PrepareTextureResourceRetirement(s.before,s.view(),selected,limits);
  const auto initialMutationBytes=s.mutation.ownedBytes();
  budget.add(initialMutationBytes);if(!replay) command.authority=s.mutation.retirementCommand();
  // This runs BEFORE export/cache invalidation or rebuilding material edges.
  // A still-selected material endpoint cannot manufacture an unused target by
  // first dropping its own serializer/provenance evidence.
  for(uint32_t runtime=0;runtime<materialCount();++runtime) if(materialIsActive(runtime)) {
    const auto live=materialResourceLiveOwner(runtime);
    if(!live || !live->active() || !live->matches(s.mutation.next()) || (retiring && !live->complete()))
      sourceFailure("An active material still references the selected source resource or has incomplete dependency authority");
  }
  if(!replay) {
    for(uint32_t runtime=0;runtime<exportTextureIndices_.size();++runtime) {
      const auto texture=exportTextureIndices_[runtime];
      budget.visit();
      if(std::binary_search(s.runtimeSlots.begin(),s.runtimeSlots.end(),runtime) ||
          (texture>=0 && selectedTextures.contains({ResourceDomainKind::Archive,uint32_t(texture)}))) {
        budget.add(sizeof(SourceResourceRetirementCommand::State::ExportDelta));command.exports.push_back({runtime,texture});
      }
    }
    budget.add(command.textures.size(),3*96);std::map<const uint8_t*,bool,std::less<>> selectedCookies;
    std::set<uint32_t> affectedArchive,affectedEvaluated;
    for(const auto& delta:command.textures) if(delta.selection.domain==ResourceDomainKind::Archive)
      {selectedCookies.emplace(archiveTextureIdentities_.at(delta.selection.row).get(),true);affectedArchive.insert(delta.selection.row);}
    else affectedEvaluated.insert(delta.selection.row);
    for(size_t index=0;index<materialTextureSourceRequests_.size();++index) {
      const auto& request=materialTextureSourceRequests_[index];if(!request.active) continue;
      const bool affectedRole=affectedArchive.contains(request.archive) || affectedEvaluated.contains(request.evaluated);
      if(affectedRole ||
          (request.inputSourceIdentity && selectedCookies.contains(request.inputSourceIdentity.get()))) {
        budget.add(sizeof(SourceResourceRetirementCommand::State::CacheDelta)+request.key.size()+128);
        if(request.encoded) budget.add(request.encoded->size());command.cache.push_back({index,request});
      }
    }
  }
  for(const auto delta:command.exports) {
    if(delta.runtime>=s.exportMapping.size() || s.exportMapping[delta.runtime]!=(retiring?delta.texture:-1))
      sourceFailure("Source replay lost its selected export mapping cell");
    s.exportMapping[delta.runtime]=retiring?-1:delta.texture;
  }
  s.requestCount=materialTextureSourceRequests_.size();budget.add(command.cache.size(),sizeof(PreparedSourceResourceRetirement::State::CacheSwap));
  s.cache.reserve(command.cache.size());
  for(const auto& delta:command.cache) {
    MaterialTextureSourceRequest tombstone;tombstone.active=false;
    const auto& expected=retiring?delta.original:tombstone;
    if(delta.index>=materialTextureSourceRequests_.size() || !sameRequest(materialTextureSourceRequests_[delta.index],expected))
      sourceFailure("Source replay lost its selected cache request cell");
    budget.add(expected.key.size()+delta.original.key.size()+128);
    s.cache.push_back({delta.index,expected,retiring?std::move(tombstone):delta.original});
  }
  // Surviving request aliases must remain exact and active. A selected request
  // is replaced by a stable empty tombstone; its vector address never shifts.
  budget.add(archiveTextureIdentities_.size(),96);std::map<const uint8_t*,uint32_t,std::less<>> identities;
  for(uint32_t row=0;row<archiveTextureIdentities_.size();++row) if(archiveTextureIdentities_[row]) identities.emplace(archiveTextureIdentities_[row].get(),row);
  size_t replacement{};
  for(size_t index=0;index<materialTextureSourceRequests_.size();++index) {
    const auto* request=&materialTextureSourceRequests_[index];
    if(replacement<s.cache.size() && s.cache[replacement].index==index) request=&s.cache[replacement++].replacement;
    budget.add(request->key.size());if(!request->active) continue;
    if(request->archive>=archiveTextureIdentities_.size() || request->sourceIdentity!=archiveTextureIdentities_[request->archive] ||
        !s.mutation.next()->active({ResourceDomainKind::Archive,ResourceKind::Texture,request->archive}) ||
        !s.mutation.next()->active({ResourceDomainKind::Evaluated,ResourceKind::Texture,request->evaluated}))
      sourceFailure("A surviving source cache request has an inactive or different destination");
    if(request->inputSourceIdentity) {
      const auto found=identities.find(request->inputSourceIdentity.get());
      if(found==identities.end() || !s.mutation.next()->active({ResourceDomainKind::Archive,ResourceKind::Texture,found->second}))
        sourceFailure("A surviving sampler cache alias has an inactive or different input");
    }
  }
  if(exportSource_.materials.size()!=sourceMaterialToRuntime_.size() || gltfModel_.materials.size()!=sourceMaterialToRuntime_.size())
    sourceFailure("Source material activity does not cover both source documents");
  budget.add(sourceMaterialToRuntime_.size());s.materialMask.reserve(sourceMaterialToRuntime_.size());
  for(const auto runtime:sourceMaterialToRuntime_) s.materialMask.push_back(materialIsActive(runtime)?1:0);
  GltfTextureResourceReferenceLimits proofLimits;
  proofLimits.policy=retiring?GltfTextureResourceReferencePolicy::RequireComplete:GltfTextureResourceReferencePolicy::RecordIncomplete;
  const auto proof=[&](const tinygltf::Model& source,const SourceResourcePhaseTable& phases) {
    proofLimits.maxOwnedBytes=std::min(proofLimits.maxOwnedBytes,budget.remaining);
    if(!proofLimits.maxOwnedBytes) sourceFailure("Source reference proof exceeds its aggregate budget");
    auto result=ExtractGltfTextureResourceReferences(source,PreparedSourceResourceRetirement::State::masks(phases,s.materialMask),proofLimits);
    budget.add(result.stats().ownedBytes);return result;
  };
  s.beforeArchiveProof=proof(exportSource_,s.beforeArchive);s.beforeEvaluatedProof=proof(gltfModel_,s.beforeEvaluated);
  s.archiveProof=proof(exportSource_,s.archive);s.evaluatedProof=proof(gltfModel_,s.evaluated);
  if(player) {
    if(player->library().restMaterials.size()<s.materialMask.size()) sourceFailure("Source resource proof has an incomplete player rest library");
    budget.add(s.materialMask.size(),sizeof(GltfMaterialRawTextureReferences));s.restProofs.resize(s.materialMask.size());
    for(size_t source=0;source<s.materialMask.size();++source) if(s.materialMask[source]) {
      proofLimits.maxOwnedBytes=std::min(proofLimits.maxOwnedBytes,budget.remaining);
      s.restProofs[source]=ExtractGltfMaterialRawTextureReferences(player->library().restMaterials[source],exportSource_.textures.size(),{},proofLimits);
      budget.add(s.restProofs[source].stats().ownedBytes);
      for(const auto texture:s.restProofs[source].textures()) if(!s.mutation.next()->active({ResourceDomainKind::Archive,ResourceKind::Texture,texture}))
        sourceFailure("An animation rest material references the selected source texture");
    }
  }
  budget.add(materialResourceDependencies_.size(),sizeof(MaterialResourceDependencyStatePtr));s.beforeDependencies=materialResourceDependencies_;
  s.dependencies=stageCurrentMaterialResourceDependencies(s.mutation,budget.remaining,{},player,{},UINT32_MAX,std::span<const int32_t>(s.exportMapping));
  budget.add(materialResourceDependencyMapBytes(s.dependencies,s.beforeDependencies));
  s.beforeReaders=RetainAllTextureResourceReferences(s.before,ResourcePinClass::GpuReader,budget.remaining);budget.add(s.beforeReaders->ownedBytes());
  s.futurePreparation=s.mutation.selectAll(ResourcePinClass::Preparation,budget.remaining);budget.add(s.futurePreparation->ownedBytes());
  s.mutation.seal();
  if(s.mutation.ownedBytes()>initialMutationBytes) budget.add(s.mutation.ownedBytes()-initialMutationBytes);
  s.futureGeneration=s.mutation.next();
  if(s.runtimeMutation && prepareGpu) {
    s.descriptors=prepareTextureDescriptorState(s.runtimeMutation->textures(),budget.remaining,MaterialResourcesLimits{}.maxMetadataBufferBytes);
    budget.add(s.descriptors->ownedBytes);
  }
  if(!replay) {
    command.bytes=sizeof(SourceResourceRetirementCommand::State)+256+command.authority.ownedBytes()+command.runtimeAuthority.ownedBytes()+
        (command.images?command.images->ownedBytes():0)+
        command.selections.capacity()*sizeof(TextureResourceSelection)+command.textures.capacity()*sizeof(SourceResourceRetirementCommand::State::TextureDelta)+
        command.exports.capacity()*sizeof(SourceResourceRetirementCommand::State::ExportDelta)+command.cache.capacity()*sizeof(SourceResourceRetirementCommand::State::CacheDelta);
    for(const auto& delta:command.cache) {command.bytes+=delta.original.key.size();if(delta.original.encoded) command.bytes+=delta.original.encoded->size();}
  }
  if(!s.matches(*this,player)) sourceFailure("Source resource retirement became stale before publication");
  return result;
}

bool SceneManager::canExchangeSourceResourceRetirement(const PreparedSourceResourceRetirement& prepared,const AnimationPlayer* player) const noexcept {
  return prepared && !prepared.state_->cpuOnly && prepared.state_->matches(*this,player) &&
      (!prepared.state_->runtimeMutation || std::ranges::any_of(prepared.state_->descriptorFrames,[](const auto& witness){return !witness.expired();}));
}
void SceneManager::exchangeSourceResourceRetirement(PreparedSourceResourceRetirement& prepared,const AnimationPlayer* player) noexcept {
  if(!canExchangeSourceResourceRetirement(prepared,player)) return;auto& s=*prepared.state_;
  if(!ExchangeTextureResourceLifecycle(s.mutation,textureResourceLifecycle_,s.view())) return;
  if(s.runtimeMutation) {
    const bool exchanged=container::material::ExchangeRuntimeTextureRetirement(*s.runtimeMutation,textureManager_);
    assert(exchanged);(void)exchanged;
    textureImageOwnership_=s.futureImages;
    exchangeTextureDescriptorState(*s.descriptors);
  }
  using std::swap;
  swap(archiveResourcePhases_,s.archive);swap(gltfResourcePhases_,s.evaluated);
  archiveTextureResources_.color.swap(s.archiveRoles.color);archiveTextureResources_.data.swap(s.archiveRoles.data);
  gltfTextureResources_.color.swap(s.evaluatedRoles.color);gltfTextureResources_.data.swap(s.evaluatedRoles.data);
  archiveTextureToEvaluated_.swap(s.correspondence);exportTextureIndices_.swap(s.exportMapping);
  for(auto& delta:s.cache) swap(materialTextureSourceRequests_[delta.index],delta.replacement);
  materialResourceDependencies_.swap(s.dependencies);
  auto& command=*s.receipt.state_;command.published=true;command.retired=s.retiring;++command.version;
  ++materialResourceRevision_;s.consumed=true;
}
} // namespace container::scene
