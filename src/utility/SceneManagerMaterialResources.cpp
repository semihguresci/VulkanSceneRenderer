#include <Container/utility/SceneManager.h>
#include <Container/utility/GltfMaterialEditRequest.h>
#include <Container/utility/GltfRuntimeMaterialAuthoring.h>
#include <Container/utility/Platform.h>
#include <Container/utility/VulkanDevice.h>
#include <Container/utility/VulkanMemoryManager.h>
#include <Container/utility/TextureSamplingPlan.h>
#include <Container/utility/TextureImageOwnership.h>
#include <Container/geometry/GltfTextureResourceReferences.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <charconv>
#include <bit>
#include <cmath>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <utility>

namespace container::scene {
namespace {
constexpr uint64_t maxSources=1'000'000,maxPreparedBytes=1024ull*1024*1024;
bool colorSlot(size_t slot,const container::material::Material& material) noexcept {
  return container::material::GltfMaterialTextureUsesColorResource(
      container::material::GltfMaterialTextureSlot(slot),material);
}
uint64_t hashEncoded(std::span<const std::byte> bytes) {
  uint64_t hash=1469598103934665603ull;for(const auto byte:bytes) {hash^=uint8_t(byte);hash*=1099511628211ull;}return hash;
}
struct Budget {
  uint64_t remaining{maxPreparedBytes};
  uint64_t values{4'000'000};
  void add(uint64_t n,uint64_t width=1) {
    if(width && n>remaining/width) throw std::length_error("Material source preparation exceeds its byte limit");remaining-=n*width;
  }
  void value(const tinygltf::Value& item,uint32_t depth=0) {
    if(depth>64 || !values) throw std::length_error("Material resource metadata exceeds its traversal limit");
    --values;add(sizeof(item));
    if(item.IsString()) add(item.Get<std::string>().size());
    else if(item.IsBinary()) add(item.Get<std::vector<unsigned char>>().size());
    else if(item.IsArray()) for(const auto& child:item.Get<tinygltf::Value::Array>()) value(child,depth+1);
    else if(item.IsObject()) for(const auto& [key,child]:item.Get<tinygltf::Value::Object>()) {add(key.size()+64);value(child,depth+1);}
  }
  template<class T> void record(const T& item) {
    if(!values) throw std::length_error("Material resource record traversal exceeds its limit");--values;add(sizeof(T)*2);
    if constexpr(requires{item.name;}) add(item.name.size()*2);
    if constexpr(requires{item.uri;}) add(item.uri.size()*2);
    if constexpr(requires{item.mimeType;}) add(item.mimeType.size()*2);
    if constexpr(requires{item.data;}) add(item.data.size()*2);
    if constexpr(requires{item.image;}) add(item.image.size()*2);
    if constexpr(requires{item.extras;}) value(item.extras);
    if constexpr(requires{item.extensions;}) for(const auto& [name,extension]:item.extensions) {add(name.size()+64);value(extension);}
    if constexpr(requires{item.extensions_json_string;}) add(item.extensions_json_string.size());
    if constexpr(requires{item.extras_json_string;}) add(item.extras_json_string.size());
  }
};
}

void SceneManager::validateMaterialResourceSource(const tinygltf::Model& source) const {
  Budget budget;
  const auto records=[&](const auto& rows) {
    if(rows.size()>maxSources) throw std::length_error("Material source resource count exceeds its limit");
    for(const auto& row:rows) budget.record(row);
  };
  records(source.buffers);records(source.bufferViews);records(source.images);records(source.samplers);records(source.textures);
  if(source.materials.size()>maxSources) throw std::length_error("Material source definitions exceed their count limit");
  container::geometry::gltf::GltfMaterialAuthoringLimits limits;limits.maxOutputBytes=maxPreparedBytes;limits.maxMetadataValues=4'000'000;
  for(const auto& row:source.materials) budget.add(container::geometry::gltf::GltfMaterialRecordStorageBytes(row,limits),2);
  if(source.extensionsUsed.size()>65536 || source.extensionsRequired.size()>65536 ||
      exportSource_.extensionsUsed.size()>65536 || gltfModel_.extensionsUsed.size()>65536)
    throw std::length_error("Material source declarations exceed their count limit");
  const auto declarations=source.extensionsUsed.size()+source.extensionsRequired.size();
  const auto existing=exportSource_.extensionsUsed.size()+gltfModel_.extensionsUsed.size()+declarations+1;
  if(declarations && existing>64'000'000/declarations) throw std::length_error("Material source declaration comparisons exceed their work limit");
  for(const auto& name:source.extensionsUsed) budget.add(name.size()+64);
  for(const auto& name:source.extensionsRequired) budget.add(name.size()+64);
}

bool SceneManager::materialTextureReferenceMatches(const MaterialTextureReference& reference,bool color,uint32_t runtime) const noexcept {
  if(!reference || reference.ownerIdentity_!=materialResourceOwnerIdentity_ || reference.color_!=color || reference.runtime_!=runtime ||
      reference.archive_>=exportSource_.textures.size() || reference.archive_>=archiveTextureIdentities_.size() ||
      reference.sourceIdentity_!=archiveTextureIdentities_[reference.archive_] || reference.archive_>=archiveTextureToEvaluated_.size()) return false;
  if(!sourceResourceActive({container::util::ResourceDomainKind::Archive,container::util::ResourceKind::Texture,reference.archive_})) return false;
  const auto& archived=color?archiveTextureResources_.color:archiveTextureResources_.data;
  if(reference.archive_>=archived.size() || archived[reference.archive_]!=runtime || !textureManager_.isTextureActive(runtime)) return false;
  const auto evaluated=archiveTextureToEvaluated_[reference.archive_];
  if(evaluated>=0 && !sourceResourceActive({container::util::ResourceDomainKind::Evaluated,container::util::ResourceKind::Texture,uint32_t(evaluated)})) return false;
  if(evaluated<0) return reference.evaluated_==UINT32_MAX;
  const auto& live=color?gltfTextureResources_.color:gltfTextureResources_.data;
  return reference.evaluated_==uint32_t(evaluated) && size_t(evaluated)<gltfModel_.textures.size() &&
      size_t(evaluated)<live.size() && live[size_t(evaluated)]==runtime;
}
SceneManager::MaterialTextureReference SceneManager::materialTextureReference(uint32_t archive,bool color) const {
  MaterialTextureReference result;result.ownerIdentity_=materialResourceOwnerIdentity_;result.archive_=archive;result.color_=color;
  const auto& roles=color?archiveTextureResources_.color:archiveTextureResources_.data;
  if(archive>=roles.size() || archive>=archiveTextureIdentities_.size() || archive>=archiveTextureToEvaluated_.size())
    throw std::invalid_argument("Material source texture identity is unavailable");
  result.sourceIdentity_=archiveTextureIdentities_[archive];result.runtime_=roles[archive];
  if(archiveTextureToEvaluated_[archive]>=0) result.evaluated_=uint32_t(archiveTextureToEvaluated_[archive]);
  if(!materialTextureReferenceMatches(result,color,result.runtime_)) throw std::invalid_argument("Material source texture role is unavailable");
  return result;
}
bool SceneManager::runtimeMaterialProvenanceMatches(const RuntimeMaterialProvenance& row,const container::material::Material& value) const noexcept {
  if(!row.known) return false;
  const auto actual=container::geometry::gltf::snapshotMaterialTextureBindings(value).runtimeIndices;
  for(size_t slot=0;slot<actual.size();++slot) {
    if(actual[slot]==UINT32_MAX) {if(row.slots[slot]) return false;}
    else if(!row.slots[slot] || !materialTextureReferenceMatches(*row.slots[slot],colorSlot(slot,value),actual[slot])) return false;
  }
  return true;
}
bool SceneManager::hasRuntimeMaterialProvenance(uint32_t runtime) const noexcept {
  const auto* value=materialManager_.getMaterial(runtime);
  return value && runtime<runtimeMaterialProvenance_.size() && runtimeMaterialProvenanceMatches(runtimeMaterialProvenance_[runtime],*value);
}
SceneManager::RuntimeMaterialProvenance SceneManager::makeRuntimeMaterialProvenance(uint32_t,
    const container::material::Material& value,const tinygltf::Material& authored,const tinygltf::Material& evaluated) const {
  const auto a=container::material::ReadGltfMaterialSourceTextures(authored,exportSource_.textures.size());
  const auto e=container::material::ReadGltfMaterialSourceTextures(evaluated,gltfModel_.textures.size());
  const auto actual=container::geometry::gltf::snapshotMaterialTextureBindings(value).runtimeIndices;
  RuntimeMaterialProvenance row;row.known=true;
  for(size_t slot=0;slot<actual.size();++slot) {
    if(actual[slot]==UINT32_MAX) {
      if(a[slot]>=0 || e[slot]>=0) throw std::invalid_argument("Material source slot has no matching resident resource");
      continue;
    }
    if(a[slot]<0 || e[slot]<0) throw std::invalid_argument("Resident material texture has no explicit source identity");
    auto reference=materialTextureReference(uint32_t(a[slot]),colorSlot(slot,value));
    if(!reference.evaluatedTexture() || *reference.evaluatedTexture()!=uint32_t(e[slot]) ||
        !materialTextureReferenceMatches(reference,colorSlot(slot,value),actual[slot]))
      throw std::invalid_argument("Authored and evaluated material source roles disagree");
    row.slots[slot]=std::move(reference);
  }
  return row;
}
void SceneManager::rememberRuntimeMaterialProvenance(uint32_t runtime,const tinygltf::Material& authored,const tinygltf::Material& evaluated) {
  const auto* value=materialManager_.getMaterial(runtime);if(!value) throw std::invalid_argument("Material source runtime ID is unavailable");
  if(materialManager_.materialCount()>maxPreparedBytes/sizeof(RuntimeMaterialProvenance)) throw std::length_error("Material provenance table exceeds its byte limit");
  runtimeMaterialProvenance_.resize(materialManager_.materialCount());
  try {runtimeMaterialProvenance_[runtime]=makeRuntimeMaterialProvenance(runtime,*value,authored,evaluated);}
  catch(const std::bad_alloc&) {throw;}
  catch(const std::exception&) {runtimeMaterialProvenance_[runtime]={};}
}
uint32_t SceneManager::createMaterial(const container::material::Material& value) {
  const auto actual=container::geometry::gltf::snapshotMaterialTextureBindings(value).runtimeIndices;
  const bool absent=std::ranges::all_of(actual,[](uint32_t id){return id==UINT32_MAX;});
  if(absent) return createMaterial(value,MaterialTextureProvenance{});
  return createMaterialTracked(value,{});
}
uint32_t SceneManager::createMaterial(const container::material::Material& value,const MaterialTextureProvenance& provenance) {
  RuntimeMaterialProvenance row{true,provenance};
  if(!runtimeMaterialProvenanceMatches(row,value)) throw std::invalid_argument("Material texture provenance does not match its explicit source/runtime roles");
  return createMaterialTracked(value,std::move(row));
}
uint32_t SceneManager::createMaterialTracked(const container::material::Material& value,RuntimeMaterialProvenance row) {
  if(materialRevision_==UINT64_MAX || materialManager_.materialCount()>=maxSources ||
      materialManager_.materialCount()+1>maxPreparedBytes/sizeof(RuntimeMaterialProvenance))
    throw std::length_error("Material provenance identity table exceeds its limit");
  runtimeMaterialProvenance_.reserve(materialManager_.materialCount()+1);
  std::optional<container::util::PreparedResourceRegistration> registration;
  std::optional<PreparedTextureResourceLifecycle> dependencyChange;
  std::vector<MaterialResourceDependencyStatePtr> dependencies;
  container::util::ResourceHandle handle;
  if(materialLifecycle_) {
    refreshTextureResourceLifecycle(container::util::ResourceRetirementPolicy::InvalidatePreparations,maxPreparedBytes);
    if(materialLifecycleHandles_.size()!=materialManager_.materialCount() || materialLifecycleNextSlot_==UINT32_MAX)
      throw std::invalid_argument("Material lifecycle identity table is incomplete or exhausted");
    materialLifecycleHandles_.reserve(materialManager_.materialCount()+1);
    const container::util::ResourceRegistrationRequest request{materialLifecycleDomain_,container::util::ResourceKind::Material,materialLifecycleNextSlot_};
    registration.emplace(materialLifecycle_->prepareIssue(std::span(&request,1)));handle=registration->resources().front();
    TextureResourceLifecycleLimits limits;limits.maxOwnedBytes=maxPreparedBytes;
    dependencyChange.emplace(PrepareTextureResourceLifecycle(textureResourceLifecycle_,textureResourceLifecycleView(),limits));
    const auto runtime=uint32_t(materialManager_.materialCount());
    dependencies=materialResourceDependencies_;dependencies.resize(size_t(runtime)+1);
    MaterialResourceDependencyInput input;input.runtime=runtime;input.source=-1;input.material=handle;
    input.value=&value;input.provenance=&row;
    dependencies[runtime]=stageMaterialResourceDependency(input,&*dependencyChange,nullptr,{},maxPreparedBytes);
    dependencyChange->seal();
    const auto bytes=materialResourceDependencyMapBytes(dependencies,materialResourceDependencies_);
    if(dependencyChange->ownedBytes()>maxPreparedBytes || bytes>maxPreparedBytes-dependencyChange->ownedBytes())
      throw std::length_error("New material live dependencies exceed their CPU budget");
    if(!materialLifecycle_->canExchange(*registration)) throw std::invalid_argument("Material lifecycle registration became stale");
    if(!materialResourceDependencyMapWillBeActive(dependencies,*dependencyChange) ||
        !CanExchangeTextureResourceLifecycle(*dependencyChange,textureResourceLifecycle_,textureResourceLifecycleView()))
      throw std::invalid_argument("New material resource dependencies became stale");
  }
  const auto runtime=materialManager_.createMaterial(value);runtimeMaterialProvenance_.resize(materialManager_.materialCount());
  runtimeMaterialProvenance_[runtime]=std::move(row);
  if(registration) {
    const bool published=materialLifecycle_->exchange(*registration);assert(published);(void)published;
    materialLifecycleHandles_.push_back(std::move(handle));++materialLifecycleNextSlot_;
    const bool resourcesPublished=ExchangeTextureResourceLifecycle(*dependencyChange,textureResourceLifecycle_,textureResourceLifecycleView());
    assert(resourcesPublished);(void)resourcesPublished;materialResourceDependencies_.swap(dependencies);
  }
  ++materialRevision_;return runtime;
}
uint32_t SceneManager::loadMaterialTexture(const std::filesystem::path& path,bool color,uint32_t sampler) {
  return loadMaterialTextureSource(path,color,sampler).runtimeResource();
}
uint32_t SceneManager::loadMaterialTextureFromBytes(const std::string& name,std::span<const std::byte> bytes,bool color,uint32_t sampler) {
  return loadMaterialTextureSourceFromBytes(name,bytes,color,sampler).runtimeResource();
}
SceneManager::MaterialTextureReference SceneManager::loadMaterialTextureSource(const std::filesystem::path& path,bool color,uint32_t sampler) {
  (void)container::material::DecodeMaterialSamplerKey({sampler});
  const auto normalized=path.lexically_normal();const auto name=container::util::pathToUtf8(normalized);
  if(name.size()>65536) throw std::length_error("Material texture path exceeds its limit");
  std::ifstream stream(normalized,std::ios::binary|std::ios::ate);
  if(!stream) throw std::runtime_error("failed to open material texture: "+name);
  const auto size=stream.tellg();if(size<=0 || uint64_t(size)>256ull*1024*1024) throw std::length_error("Material texture encoded bytes exceed their limit");
  std::vector<std::byte> bytes(static_cast<size_t>(size));stream.seekg(0);
  if(!stream.read(reinterpret_cast<char*>(bytes.data()),std::streamsize(bytes.size()))) throw std::runtime_error("failed to read material texture: "+name);
  return loadMaterialTextureSourceBytes("file:"+name,name,bytes,color,sampler);
}
SceneManager::MaterialTextureReference SceneManager::loadMaterialTextureSourceFromBytes(const std::string& name,std::span<const std::byte> bytes,bool color,uint32_t sampler) {
  const auto normalized=name.empty()?std::string("embedded-usd-texture"):name;
  if(normalized.size()>65536) throw std::length_error("Material texture name exceeds its limit");
  return loadMaterialTextureSourceBytes("bytes:"+normalized,normalized,bytes,color,sampler);
}

SceneManager::MaterialTextureReference SceneManager::loadMaterialTextureSourceBytes(const std::string& request,
    const std::string& name,std::span<const std::byte> bytes,bool color,uint32_t sampler) {
  if(bytes.empty() || bytes.size()>256ull*1024*1024) throw std::length_error("Material texture encoded bytes exceed their limit");
  (void)container::material::DecodeMaterialSamplerKey({sampler});
  const auto key=request+"#sampler="+std::to_string(sampler)+"#size="+std::to_string(bytes.size())+"#bytes="+std::to_string(hashEncoded(bytes));
  if(materialTextureSourceRequests_.size()>maxSources) throw std::length_error("Material texture request table exceeds its limit");
  // The caller's encoded payload and the optional immutable request copy can
  // coexist. Resident image tables are charged separately below; codec-private
  // scratch and driver allocation padding are outside these data-size limits.
  Budget budget;budget.add(bytes.size(),2);uint64_t comparisons=0;
  std::optional<uint32_t> existing;
  for(const auto& cached:materialTextureSourceRequests_) {
    budget.add(sizeof(cached)+cached.key.size()+64);
    if(!cached.active) continue;
    if(cached.key!=key) continue;
    if(bytes.size()>maxPreparedBytes-comparisons) throw std::length_error("Material texture request comparison exceeds its limit");comparisons+=bytes.size();
    if(cached.encoded && cached.encoded->size()==bytes.size() && std::equal(bytes.begin(),bytes.end(),cached.encoded->begin())) {
      if(cached.archive>=archiveTextureIdentities_.size() || cached.sourceIdentity!=archiveTextureIdentities_[cached.archive])
        throw std::invalid_argument("Material texture request source identity is stale");
      if(!sourceResourceActive({container::util::ResourceDomainKind::Archive,container::util::ResourceKind::Texture,cached.archive})) continue;
      existing=cached.archive;break;
    }
  }
  if(existing) {
    const auto& role=color?archiveTextureResources_.color:archiveTextureResources_.data;
    if(*existing<role.size() && role[*existing]!=UINT32_MAX) return materialTextureReference(*existing,color);
  }
  bool archivePrefix=gltfModel_.buffers.size()<=exportSource_.buffers.size() && gltfModel_.bufferViews.size()<=exportSource_.bufferViews.size() &&
      gltfModel_.images.size()<=exportSource_.images.size() && gltfModel_.samplers.size()<=exportSource_.samplers.size() && gltfModel_.textures.size()<=exportSource_.textures.size();
  for(size_t i=0;archivePrefix && i<gltfModel_.textures.size();++i)
    archivePrefix=i<archiveTextureToEvaluated_.size() && archiveTextureToEvaluated_[i]==
        (sourceResourceActive({container::util::ResourceDomainKind::Archive,container::util::ResourceKind::Texture,uint32_t(i)})?int32_t(i):-1);
  if(!archivePrefix && !sourceMaterialToRuntime_.empty() && materialBuffer_.buffer!=VK_NULL_HANDLE && textureMetadataBuffer_.buffer!=VK_NULL_HANDLE &&
      !sceneClipBuffers().empty() && materialSamplers_.size()==container::material::kMaterialSamplerKeyCount) {
    // Editor resource promotion permits independent authored/evaluated image
    // domains. Reuse its detached path when a later BIM immediate import can
    // no longer use the old initialization-only archive-prefix promotion.
    auto staged=prepareMaterialResourcesBase(0,{},true);
    staged.prepareSource({},request,name,bytes,color?MaterialTextureRoles::Color:MaterialTextureRoles::Data,sampler);staged.finishPreparation();
    const auto reference=staged.reference(color);
    staged.sealLiveDependencies();
    if(!canExchangeMaterialResources(staged)) throw std::invalid_argument("Immediate texture resource scope became stale");
    exchangeMaterialResources(staged);return reference;
  }
  if(materialResourceRevision_==UINT64_MAX) throw std::length_error("Material resource revision is exhausted");
  if(textureManager_.textureCount()>=textureDescriptorCapacity_)
    throw std::length_error("Material texture exceeds the sampled-image descriptor capacity");
  if(textureManager_.textureCount()>=maxSources || exportSource_.textures.size()>=maxSources || gltfModel_.textures.size()>=maxSources ||
      exportSource_.images.size()>=maxSources || gltfModel_.images.size()>=maxSources || exportSource_.samplers.size()>=maxSources || gltfModel_.samplers.size()>=maxSources)
    throw std::length_error("Material texture source/resource count exceeds its limit");
  validateMaterialSamplerPolicies(std::array{container::material::MaterialSamplerKey{sampler}});
  for(uint32_t i=0;i<textureManager_.textureCount();++i) {
    budget.add(container::material::TextureManager::rowStorageBytes()*4+256);
    if(const auto* resource=textureManager_.getTexture(i)) budget.add(resource->name.size()*3+resource->contentKey.size()*3);
  }
  budget.add(exportSource_.textures.size()+gltfModel_.textures.size()+1,64);
  const auto ownershipBytes=container::material::TextureImageOwnershipStorageBytes(textureManager_.textureCount()+1);
  budget.add(ownershipBytes);
  budget.add(sizeof(container::gpu::AllocationManager::TextureAllocationLease)+128);
  budget.add(runtimeMaterialProvenance_.size(),sizeof(RuntimeMaterialProvenance));
  if(!container::material::TextureImageOwnershipMatches(textureImageOwnership_,textureManager_,*allocationManager_))
    throw std::invalid_argument("Immediate material texture producer has incomplete live image ownership");
  auto candidateTextures=textureManager_;
  auto candidateRequests=materialTextureSourceRequests_;
  auto candidateImageOwnership=textureImageOwnership_;
  auto candidateProvenance=runtimeMaterialProvenance_;
  auto decode=allocationManager_->textureDecodeOptions(color);
  if(!decode.maxDecodedBytes || decode.maxDecodedBytes==UINT64_MAX || !decode.maxEncodedBytes || decode.maxEncodedBytes==UINT64_MAX ||
      decode.maxMipLevels>32 || decode.allowedFormats.size()>1024)
    throw std::invalid_argument("Material texture decoding requires finite device capacities");
  budget.add(sizeof(container::material::TextureImageData)+decode.maxMipLevels*sizeof(container::material::TextureMipLevel)+
      decode.allowedFormats.size()*sizeof(VkFormat));
  // Bound both the decoded CPU stream and its transient VMA staging payload
  // before decoding allocates either. Upload consumes this prepared stream and
  // does not perform another image decode.
  decode.maxDecodedBytes=std::min(decode.maxDecodedBytes,budget.remaining/2);
  if(!decode.maxDecodedBytes) throw std::length_error("Material texture has no remaining decoded byte capacity");
  const auto preparedImage=container::material::DecodeTextureImage(bytes,decode);
  uint64_t decodedBytes{};
  for(const auto& mip:preparedImage.mips) {
    if(mip.bytes.size()>decode.maxDecodedBytes-decodedBytes) throw std::length_error("Decoded material image exceeds its aggregate byte limit");
    decodedBytes+=mip.bytes.size();
  }
  budget.add(decodedBytes,2);
  auto retention=decode;
  const auto raw=std::span(reinterpret_cast<const unsigned char*>(bytes.data()),bytes.size());
  const bool encodedArchive=!container::geometry::gltf::EncodedTextureMimeType(raw).empty();
  const auto archiveCapacity=encodedArchive?uint64_t(bytes.size()):std::min<uint64_t>(retention.maxDecodedBytes,budget.remaining/2);
  if(!archiveCapacity) throw std::length_error("Material texture has no remaining archive byte capacity");
  budget.add(archiveCapacity,2);
  retention.maxDecodedBytes=archiveCapacity;
  auto image=container::geometry::gltf::RetainTextureImageForExport(bytes,retention);
  if(image.image.size()>archiveCapacity) throw std::length_error("Retained material image exceeds its prepared byte limit");
  // Keep the measured selected-record charge after the bounded fallback decode.
  budget.remaining+=(archiveCapacity-image.image.size())*2;
  const auto archived=existing.value_or(uint32_t(exportSource_.textures.size()));
  const auto live=existing?uint32_t(archiveTextureToEvaluated_.at(archived)):uint32_t(exportSource_.textures.size());
  if(existing && (archiveTextureToEvaluated_[archived]<0 || live>=gltfModel_.textures.size()))
    throw std::invalid_argument("Material source request has no matching evaluated resource identity");
  const auto oldAImages=exportSource_.images.size(),oldASamplers=exportSource_.samplers.size(),oldATextures=exportSource_.textures.size();
  const auto oldEImages=gltfModel_.images.size(),oldESamplers=gltfModel_.samplers.size(),oldETextures=gltfModel_.textures.size();
  const auto oldEBuffers=gltfModel_.buffers.size(),oldEViews=gltfModel_.bufferViews.size();
  const auto oldUsed=gltfModel_.extensionsUsed.size(),oldRequired=gltfModel_.extensionsRequired.size();
  const auto oldArchiveRoles=archiveTextureResources_.color.size();
  const auto oldArchiveDataRoles=archiveTextureResources_.data.size();
  auto previousMap=archiveTextureToEvaluated_;
  auto previousLiveRoles=gltfTextureResources_;
  auto previousIdentities=archiveTextureIdentities_;
  try {
    // Promote only missing resource prefixes. Their indices stay identical to
    // the archive, so later merge plans can retain the existing live prefix.
    // Geometry, materials, nodes, skins and curves are never copied here.
    if(gltfModel_.buffers.size()>exportSource_.buffers.size() || gltfModel_.bufferViews.size()>exportSource_.bufferViews.size() ||
        oldEImages>oldAImages || oldESamplers>oldASamplers || oldETextures>oldATextures)
      throw std::invalid_argument("Material resource domains are not a promotable archive prefix");
    for(size_t i=0;i<oldETextures;++i)
      if(i>=archiveTextureToEvaluated_.size() || archiveTextureToEvaluated_[i]!=
          (sourceResourceActive({container::util::ResourceDomainKind::Archive,container::util::ResourceKind::Texture,uint32_t(i)})?int32_t(i):-1))
        throw std::invalid_argument("Material evaluated resource prefix has no explicit archive identity");
    const auto preflightSuffix=[&](const auto& current,const auto& archive) {
      if(archive.size()>maxSources) throw std::length_error("Material resource prefix exceeds its count limit");
      for(size_t i=current.size();i<archive.size();++i) budget.record(archive[i]);
    };
    preflightSuffix(gltfModel_.buffers,exportSource_.buffers);preflightSuffix(gltfModel_.bufferViews,exportSource_.bufferViews);
    preflightSuffix(gltfModel_.images,exportSource_.images);preflightSuffix(gltfModel_.samplers,exportSource_.samplers);
    preflightSuffix(gltfModel_.textures,exportSource_.textures);
    if(exportSource_.extensionsUsed.size()>maxSources || exportSource_.extensionsRequired.size()>maxSources)
      throw std::length_error("Material resource declarations exceed their count limit");
    for(const auto& name:exportSource_.extensionsUsed) budget.add(name.size()+64);
    for(const auto& name:exportSource_.extensionsRequired) budget.add(name.size()+64);
    const auto appendSuffix=[](auto& current,const auto& archive) {
      current.insert(current.end(),archive.begin()+current.size(),archive.end());
    };
    appendSuffix(gltfModel_.buffers,exportSource_.buffers);appendSuffix(gltfModel_.bufferViews,exportSource_.bufferViews);
    appendSuffix(gltfModel_.images,exportSource_.images);appendSuffix(gltfModel_.samplers,exportSource_.samplers);
    appendSuffix(gltfModel_.textures,exportSource_.textures);
    gltfTextureResources_.color.resize(oldATextures,UINT32_MAX);gltfTextureResources_.data.resize(oldATextures,UINT32_MAX);
    archiveTextureToEvaluated_.resize(oldATextures,-1);archiveTextureIdentities_.resize(oldATextures);
    for(size_t i=oldETextures;i<oldATextures;++i) {
      if(i>=archiveTextureResources_.color.size() || i>=archiveTextureResources_.data.size())
        throw std::invalid_argument("Archive material resource roles are incomplete");
      gltfTextureResources_.color[i]=archiveTextureResources_.color[i];gltfTextureResources_.data[i]=archiveTextureResources_.data[i];
      archiveTextureToEvaluated_[i]=sourceResourceActive({container::util::ResourceDomainKind::Archive,container::util::ResourceKind::Texture,uint32_t(i)})?int32_t(i):-1;
      if(!archiveTextureIdentities_[i]) archiveTextureIdentities_[i]=std::make_shared<const uint8_t>(0);
    }
    const auto addDeclarations=[&](auto& current,const auto& names) {
      for(const auto& name:names) {
        bool found=false;
        for(const auto& previous:current) {
          if(!budget.values) throw std::length_error("Material source declaration comparison exceeds its work limit");
          --budget.values;if(previous==name) {found=true;break;}
        }
        if(!found) current.push_back(name);
      }
    };
    addDeclarations(gltfModel_.extensionsUsed,exportSource_.extensionsUsed);addDeclarations(gltfModel_.extensionsRequired,exportSource_.extensionsRequired);
    if(!existing) {
      auto cookie=std::make_shared<const uint8_t>(0);
      auto encoded=std::make_shared<const std::vector<std::byte>>(bytes.begin(),bytes.end());
      candidateRequests.push_back({key,std::move(encoded),archived,live,cookie});
      tinygltf::Sampler sourceSampler;const auto policy=container::material::DecodeMaterialSamplerKey({sampler});
      sourceSampler.minFilter=policy.minFilter;sourceSampler.magFilter=policy.magFilter;sourceSampler.wrapS=policy.wrapS;sourceSampler.wrapT=policy.wrapT;
      tinygltf::Texture aTexture,eTexture;aTexture.source=int(oldAImages);aTexture.sampler=int(oldASamplers);
      eTexture.source=int(gltfModel_.images.size());eTexture.sampler=int(gltfModel_.samplers.size());
      exportSource_.images.push_back(image);gltfModel_.images.push_back(std::move(image));
      exportSource_.samplers.push_back(sourceSampler);gltfModel_.samplers.push_back(std::move(sourceSampler));
      exportSource_.textures.push_back(std::move(aTexture));gltfModel_.textures.push_back(std::move(eTexture));
      archiveTextureResources_.color.resize(exportSource_.textures.size(),UINT32_MAX);archiveTextureResources_.data.resize(exportSource_.textures.size(),UINT32_MAX);
      gltfTextureResources_.color.resize(gltfModel_.textures.size(),UINT32_MAX);gltfTextureResources_.data.resize(gltfModel_.textures.size(),UINT32_MAX);
      archiveTextureToEvaluated_.resize(exportSource_.textures.size(),-1);archiveTextureToEvaluated_[archived]=int32_t(live);
      archiveTextureIdentities_.resize(exportSource_.textures.size());archiveTextureIdentities_[archived]=std::move(cookie);
    }
    auto batch=allocationManager_->beginTextureAllocationBatch();
    auto resource=allocationManager_->createTextureFromImageData(name,preparedImage,materialQueueFamilies());
    resource.name=key+"#source="+std::to_string(archived)+(color?"#color":"#data");resource.samplerIndex=sampler;
    if(candidateTextures.findTextureIndex(resource.name)) throw std::invalid_argument("Material source resource name is already registered without this proof");
    const auto runtime=candidateTextures.registerTexture(resource);
    auto candidateExportIndices=exportTextureIndices_;candidateExportIndices.resize(candidateTextures.textureCount(),-1);candidateExportIndices[runtime]=int32_t(archived);
    MaterialTextureReference result;result.ownerIdentity_=materialResourceOwnerIdentity_;
    result.sourceIdentity_=archiveTextureIdentities_.at(archived);result.runtime_=runtime;result.archive_=archived;result.evaluated_=live;result.color_=color;
    if(!result.sourceIdentity_) throw std::invalid_argument("Material source identity cookie is unavailable");
    auto aggregate=batch.commit();
    const std::array inherited{textureImageOwnership_};
    candidateImageOwnership=container::material::PrepareTextureImageOwnership(candidateTextures,*allocationManager_,inherited,ownershipBytes);
    for(uint32_t material=0;material<candidateProvenance.size();++material) {
      auto& row=candidateProvenance[material];if(!row.known) continue;
      const auto* value=materialManager_.getMaterial(material);if(!value) {row={};continue;}
      const auto bindings=container::geometry::gltf::snapshotMaterialTextureBindings(*value).runtimeIndices;
      bool valid=true;
      for(size_t slot=0;slot<bindings.size();++slot) {
        if(bindings[slot]==UINT32_MAX) {if(row.slots[slot]) valid=false;continue;}
        if(!row.slots[slot]) {valid=false;break;}
        auto& old=*row.slots[slot];const auto& roles=colorSlot(slot,*value)?archiveTextureResources_.color:archiveTextureResources_.data;
        if(old.ownerIdentity_!=materialResourceOwnerIdentity_ || old.archive_>=roles.size() || old.archive_>=archiveTextureIdentities_.size() ||
            old.archive_>=archiveTextureToEvaluated_.size() || old.sourceIdentity_!=archiveTextureIdentities_[old.archive_] ||
            old.color_!=colorSlot(slot,*value) || old.runtime_!=bindings[slot] || roles[old.archive_]!=bindings[slot]) {valid=false;break;}
        if(old.archive_>=previousMap.size()) {valid=false;break;}
        const auto previous=previousMap[old.archive_];const auto& liveRoles=colorSlot(slot,*value)?previousLiveRoles.color:previousLiveRoles.data;
        if(previous<0 ? old.evaluated_!=UINT32_MAX : old.evaluated_!=uint32_t(previous) || size_t(previous)>=liveRoles.size() || liveRoles[size_t(previous)]!=bindings[slot]) {valid=false;break;}
        old.evaluated_=archiveTextureToEvaluated_[old.archive_]<0?UINT32_MAX:uint32_t(archiveTextureToEvaluated_[old.archive_]);
      }
      if(!valid) row={};
    }
    budget.add(archiveTextureResources_.color.size()+archiveTextureResources_.data.size()+
        gltfTextureResources_.color.size()+gltfTextureResources_.data.size(),sizeof(uint32_t));
    auto candidateArchiveRoles=archiveTextureResources_,candidateEvaluatedRoles=gltfTextureResources_;
    const auto phaseBytes=archiveResourcePhases_.images.size()+archiveResourcePhases_.textures.size()+archiveResourcePhases_.samplers.size()+
        gltfResourcePhases_.images.size()+gltfResourcePhases_.textures.size()+gltfResourcePhases_.samplers.size();
    budget.add(phaseBytes);
    auto candidateArchivePhases=archiveResourcePhases_,candidateEvaluatedPhases=gltfResourcePhases_;
    const auto extend=[&](auto& phases,size_t imageCount,size_t textureCount,size_t samplerCount) {
      if(!phases.images.empty() && imageCount>phases.images.size()) budget.add(imageCount-phases.images.size());
      if(!phases.textures.empty() && textureCount>phases.textures.size()) budget.add(textureCount-phases.textures.size());
      if(!phases.samplers.empty() && samplerCount>phases.samplers.size()) budget.add(samplerCount-phases.samplers.size());
      phases.resize(imageCount,textureCount,samplerCount);
    };
    extend(candidateArchivePhases,exportSource_.images.size(),exportSource_.textures.size(),exportSource_.samplers.size());
    extend(candidateEvaluatedPhases,gltfModel_.images.size(),gltfModel_.textures.size(),gltfModel_.samplers.size());
    const auto promoted=[&](auto& destination,const auto& archived,size_t oldCount,size_t promotedCount,size_t count) {
      if(archived.empty()) return;
      for(size_t i=oldCount;i<promotedCount;++i) if(archived[i]==0) {
        if(destination.empty()) {budget.add(count);destination.assign(count,1);}
        destination[i]=0;
      }
    };
    promoted(candidateEvaluatedPhases.images,candidateArchivePhases.images,oldEImages,oldAImages,gltfModel_.images.size());
    promoted(candidateEvaluatedPhases.textures,candidateArchivePhases.textures,oldETextures,oldATextures,gltfModel_.textures.size());
    promoted(candidateEvaluatedPhases.samplers,candidateArchivePhases.samplers,oldESamplers,oldASamplers,gltfModel_.samplers.size());
    (color?candidateArchiveRoles.color:candidateArchiveRoles.data)[archived]=runtime;
    (color?candidateEvaluatedRoles.color:candidateEvaluatedRoles.data)[live]=runtime;
    const TextureResourceLifecycleView logicalView{
        {exportSource_.images,exportSource_.textures,exportSource_.samplers,exportSource_.bufferViews,exportSource_.buffers,
            candidateArchiveRoles.color,candidateArchiveRoles.data,true,candidateArchivePhases.images,candidateArchivePhases.textures,candidateArchivePhases.samplers},
        {gltfModel_.images,gltfModel_.textures,gltfModel_.samplers,gltfModel_.bufferViews,gltfModel_.buffers,
            candidateEvaluatedRoles.color,candidateEvaluatedRoles.data,true,candidateEvaluatedPhases.images,candidateEvaluatedPhases.textures,candidateEvaluatedPhases.samplers},candidateTextures,archiveTextureToEvaluated_,archiveTextureIdentities_};
    TextureResourceLifecycleLimits logicalLimits;logicalLimits.maxOwnedBytes=budget.remaining;
    auto logical=PrepareTextureResourceLifecycle(textureResourceLifecycle_,logicalView,logicalLimits);
    const auto logicalBytesBefore=logical.ownedBytes();budget.add(logicalBytesBefore);
    auto candidateDependencies=stageCurrentMaterialResourceDependencies(logical,budget.remaining,{},nullptr,candidateProvenance);
    budget.add(materialResourceDependencyMapBytes(candidateDependencies,materialResourceDependencies_));
    if(logical.ownedBytes()>logicalBytesBefore) budget.add(logical.ownedBytes()-logicalBytesBefore);
    logical.seal();
    if(!materialResourceDependencyMapWillBeActive(candidateDependencies,logical) ||
        !CanExchangeTextureResourceLifecycle(logical,textureResourceLifecycle_,logicalView))
      throw std::invalid_argument("Immediate resource logical registration became stale");
    // Every candidate alias now has an independent image pin. Releasing the
    // rollback batch can synchronize/free, so it belongs before publication.
    aggregate.reset();
    const bool logicalPublished=ExchangeTextureResourceLifecycle(logical,textureResourceLifecycle_,logicalView);
    assert(logicalPublished);(void)logicalPublished;
    using std::swap;swap(textureManager_,candidateTextures);textureImageOwnership_.swap(candidateImageOwnership);
    materialTextureSourceRequests_.swap(candidateRequests);exportTextureIndices_.swap(candidateExportIndices);
    swap(archiveTextureResources_,candidateArchiveRoles);swap(gltfTextureResources_,candidateEvaluatedRoles);
    swap(archiveResourcePhases_,candidateArchivePhases);swap(gltfResourcePhases_,candidateEvaluatedPhases);
    materialResourceDependencies_.swap(candidateDependencies);
    runtimeMaterialProvenance_.swap(candidateProvenance);
    ++materialResourceRevision_;materialResourceVacancy_.reset();
    return result;
  } catch(...) {
    exportSource_.images.resize(oldAImages);exportSource_.samplers.resize(oldASamplers);exportSource_.textures.resize(oldATextures);
    gltfModel_.images.resize(oldEImages);gltfModel_.samplers.resize(oldESamplers);gltfModel_.textures.resize(oldETextures);
    gltfModel_.buffers.resize(oldEBuffers);gltfModel_.bufferViews.resize(oldEViews);
    gltfModel_.extensionsUsed.resize(oldUsed);gltfModel_.extensionsRequired.resize(oldRequired);
    archiveTextureResources_.color.resize(oldArchiveRoles);archiveTextureResources_.data.resize(oldArchiveDataRoles);
    gltfTextureResources_.color.swap(previousLiveRoles.color);gltfTextureResources_.data.swap(previousLiveRoles.data);
    archiveTextureToEvaluated_.swap(previousMap);archiveTextureIdentities_.swap(previousIdentities);throw;
  }
}
} // namespace container::scene

namespace container::scene {
namespace {
using ResourceImageOwnership = container::material::TextureImageOwnershipPtr;
struct ResourceBudget {
  uint64_t remaining{},values{};
  uint32_t depth{};
  void add(uint64_t count,uint64_t width=1) {
    if(width && count>remaining/width) throw std::length_error("Material resource generation exceeds its CPU budget");
    remaining-=count*width;
  }
  void value(const tinygltf::Value& item,uint32_t level=0) {
    if(level>depth || !values) throw std::length_error("Material resource metadata exceeds its work/depth budget");
    --values;add(sizeof(item));
    if(item.IsString()) add(item.Get<std::string>().size());
    else if(item.IsBinary()) add(item.Get<std::vector<unsigned char>>().size());
    else if(item.IsArray()) for(const auto& child:item.Get<tinygltf::Value::Array>()) value(child,level+1);
    else if(item.IsObject()) for(const auto& [name,child]:item.Get<tinygltf::Value::Object>()) {add(name.size()+64);value(child,level+1);}
  }
  template<class T> void record(const T& item,uint64_t copies=1) {
    if(!values) throw std::length_error("Material resource metadata exceeds its work budget");--values;
    add(sizeof(T),copies);
    if constexpr(requires{item.name;}) add(item.name.size(),copies);
    if constexpr(requires{item.uri;}) add(item.uri.size(),copies);
    if constexpr(requires{item.mimeType;}) add(item.mimeType.size(),copies);
    if constexpr(requires{item.image;}) add(item.image.size(),copies);
    if constexpr(requires{item.extras;}) for(uint64_t i=0;i<copies;++i) value(item.extras);
    if constexpr(requires{item.extensions;}) for(uint64_t i=0;i<copies;++i)
      for(const auto& [name,extension]:item.extensions) {add(name.size()+64);value(extension);}
    if constexpr(requires{item.extensions_json_string;}) add(item.extensions_json_string.size(),copies);
    if constexpr(requires{item.extras_json_string;}) add(item.extras_json_string.size(),copies);
  }
};
bool sameTexture(const container::material::TextureResource& a,const container::material::TextureResource& b) noexcept {
  return a.name==b.name && a.image==b.image && a.imageView==b.imageView && a.sampler==b.sampler &&
      a.samplerIndex==b.samplerIndex && a.format==b.format && a.width==b.width && a.height==b.height &&
      a.mipLevels==b.mipLevels && a.contentKey==b.contentKey && a.allocationIdentity==b.allocationIdentity;
}
bool sameTextures(const container::material::TextureManager& a,const container::material::TextureManager& b) noexcept {
  if(a.textureCount()!=b.textureCount() || a.activeTextureCount()!=b.activeTextureCount() || a.revision()!=b.revision()) return false;
  for(uint32_t i=0;i<a.textureCount();++i) {
    const auto* left=a.getTexture(i);const auto* right=b.getTexture(i);
    if(bool(left)!=bool(right) || (left && !sameTexture(*left,*right))) return false;
  }
  return true;
}
bool resourceRole(MaterialTextureRoles roles,bool color) noexcept {
  return (uint8_t(roles)&(color?1u:2u))!=0;
}
void validateResourceLimits(const MaterialResourcesLimits& limits) {
  for(uint64_t count:{limits.maxPreparedCpuBytes,limits.maxEncodedBytes,limits.maxNewTextureBytes,
      limits.maxMetadataBufferBytes,limits.maxSourceCount,limits.maxMetadataValues})
    if(!count || count==UINT64_MAX) throw std::invalid_argument("Material resource generation requires finite capacities");
  if(limits.maxSourceCount>INT32_MAX || !limits.maxMetadataDepth || limits.maxMetadataDepth>128)
    throw std::invalid_argument("Material resource identity/depth capacity is invalid");
}
uint64_t uploadedImageBytes(const container::material::TextureImageData& image) {
  if(image.mips.empty()) throw std::invalid_argument("Prepared material image has no mip levels");
  uint64_t bytes{};
  if(image.authoredMipChain || image.mips.size()>1) {
    for(const auto& mip:image.mips) {
      if(mip.bytes.size()>UINT64_MAX-bytes) throw std::length_error("Material texture byte accounting overflows");
      bytes+=mip.bytes.size();
    }
  } else {
    uint32_t width=image.mips[0].width,height=image.mips[0].height;
    while(width && height) {
      const auto level=container::material::TextureMipByteSize(image.format,width,height);
      if(level>UINT64_MAX-bytes) throw std::length_error("Material texture byte accounting overflows");bytes+=level;
      if(width==1 && height==1) break;
      width=std::max(1u,width/2);height=std::max(1u,height/2);
    }
  }
  return bytes;
}
enum class ResourceDomain:uint8_t {Other,Root,Material,Primitive,Node,Texture,TextureInfo,View,Accessor,Target,Light};
bool resourceExtensionAllowed(std::string_view name,ResourceDomain domain) noexcept {
  const bool area=name=="KHR_lights_area" || name=="EXT_lights_area" || name=="WEBGI_lights_area" || name=="WEBGI_area_light";
  if(name=="KHR_xmp_json_ld") return true; // Package references are metadata, not texture/image/sampler IDs.
  switch(domain) {
    case ResourceDomain::Root:return name=="KHR_materials_variants" || name=="KHR_lights_punctual" || area;
    case ResourceDomain::Material:
      for(const auto known:{"KHR_materials_unlit","KHR_materials_emissive_strength","KHR_materials_ior","KHR_materials_dispersion",
          "KHR_materials_transmission","KHR_materials_volume","KHR_materials_clearcoat","KHR_materials_sheen","KHR_materials_specular",
          "KHR_materials_iridescence","KHR_materials_anisotropy","KHR_materials_diffuse_transmission","KHR_materials_pbrSpecularGlossiness","MSFT_lod"})
        if(name==known) return true;
      return false;
    case ResourceDomain::Primitive:return name=="KHR_materials_variants" || name=="KHR_draco_mesh_compression";
    case ResourceDomain::Node:return name=="KHR_lights_punctual" || area || name=="EXT_mesh_gpu_instancing" || name=="MSFT_lod" ||
        name=="KHR_node_visibility" || name=="KHR_node_selectability" || name=="KHR_node_hoverability";
    case ResourceDomain::Texture:return name=="KHR_texture_basisu" || name=="EXT_texture_webp" || name=="MSFT_texture_dds";
    case ResourceDomain::TextureInfo:return name=="KHR_texture_transform";
    case ResourceDomain::View:return name=="EXT_meshopt_compression" || name=="KHR_meshopt_compression";
    case ResourceDomain::Accessor:return name=="KHR_accessor_float64";
    case ResourceDomain::Target:return name=="KHR_animation_pointer";
    case ResourceDomain::Light:return area;
    default:return false;
  }
}
bool sameResourceValue(const tinygltf::Value& a,const tinygltf::Value& b) noexcept {
  if(a.Type()!=b.Type() || a.Get<int>()!=b.Get<int>() || a.Get<bool>()!=b.Get<bool>() ||
      std::bit_cast<uint64_t>(a.Get<double>())!=std::bit_cast<uint64_t>(b.Get<double>()) ||
      a.Get<std::string>()!=b.Get<std::string>() || a.Get<std::vector<unsigned char>>()!=b.Get<std::vector<unsigned char>>()) return false;
  const auto& aa=a.Get<tinygltf::Value::Array>();const auto& ba=b.Get<tinygltf::Value::Array>();
  if(aa.size()!=ba.size()) return false;
  for(size_t i=0;i<aa.size();++i) if(!sameResourceValue(aa[i],ba[i])) return false;
  const auto& ao=a.Get<tinygltf::Value::Object>();const auto& bo=b.Get<tinygltf::Value::Object>();
  if(ao.size()!=bo.size()) return false;auto bi=bo.begin();
  for(const auto& [name,value]:ao) {if(name!=bi->first || !sameResourceValue(value,bi->second)) return false;++bi;}
  return true;
}
bool sameResourceExtensions(const tinygltf::ExtensionMap& a,const tinygltf::ExtensionMap& b) noexcept {
  if(a.size()!=b.size()) return false;auto bi=b.begin();
  for(const auto& [name,value]:a) {if(name!=bi->first || !sameResourceValue(value,bi->second)) return false;++bi;}
  return true;
}
template<class T> bool sameResourceRecord(const T& a,const T& b) noexcept {
  // TinyGLTF's operator== copies Value object maps and omits several image
  // format/cache fields. Publication must compare resident storage directly.
  if constexpr(requires{a.name;}) if(a.name!=b.name) return false;
  if constexpr(requires{a.uri;}) if(a.uri!=b.uri) return false;
  if constexpr(requires{a.mimeType;}) if(a.mimeType!=b.mimeType) return false;
  if constexpr(requires{a.image;}) if(a.image!=b.image || a.width!=b.width || a.height!=b.height || a.component!=b.component ||
      a.bits!=b.bits || a.pixel_type!=b.pixel_type || a.as_is!=b.as_is || a.bufferView!=b.bufferView) return false;
  if constexpr(requires{a.minFilter;}) if(a.minFilter!=b.minFilter || a.magFilter!=b.magFilter || a.wrapS!=b.wrapS || a.wrapT!=b.wrapT) return false;
  if constexpr(requires{a.sampler;}) if(a.sampler!=b.sampler || a.source!=b.source) return false;
  if constexpr(requires{a.extras;}) if(!sameResourceValue(a.extras,b.extras)) return false;
  if constexpr(requires{a.extensions;}) if(!sameResourceExtensions(a.extensions,b.extensions)) return false;
  if constexpr(requires{a.extras_json_string;}) if(a.extras_json_string!=b.extras_json_string) return false;
  if constexpr(requires{a.extensions_json_string;}) if(a.extensions_json_string!=b.extensions_json_string) return false;
  return true;
}
template<class T> bool sameResourceRecords(const std::vector<T>& a,const std::vector<T>& b) noexcept {
  if(a.size()!=b.size()) return false;
  for(size_t i=0;i<a.size();++i) if(!sameResourceRecord(a[i],b[i])) return false;
  return true;
}
template<class Visitor> bool walkResourceExtensions(const tinygltf::Model& model,Visitor&& visitor) {
  const auto location=[](uint64_t kind,size_t index=0,size_t child=0) noexcept {return (kind<<56)|(uint64_t(index)<<28)|uint64_t(child);};
  const auto property=[&](ResourceDomain domain,const auto& item,uint64_t key)->bool {
    if constexpr(requires{item.extensions;}) {
      std::string_view raw;
      if constexpr(requires{item.extensions_json_string;}) raw=item.extensions_json_string;
      return visitor(domain,item.extensions,raw,key);
    }
    return true;
  };
  if(!property(ResourceDomain::Root,model,location(1)) || !property(ResourceDomain::Other,model.asset,location(2))) return false;
  for(size_t i=0;i<model.buffers.size();++i) if(!property(ResourceDomain::Other,model.buffers[i],location(3,i))) return false;
  for(size_t i=0;i<model.bufferViews.size();++i) if(!property(ResourceDomain::View,model.bufferViews[i],location(4,i))) return false;
  for(size_t i=0;i<model.accessors.size();++i) if(!property(ResourceDomain::Accessor,model.accessors[i],location(5,i))) return false;
  for(size_t i=0;i<model.cameras.size();++i) if(!property(ResourceDomain::Other,model.cameras[i],location(6,i))) return false;
  for(size_t i=0;i<model.scenes.size();++i) if(!property(ResourceDomain::Other,model.scenes[i],location(7,i))) return false;
  for(size_t i=0;i<model.nodes.size();++i) if(!property(ResourceDomain::Node,model.nodes[i],location(8,i))) return false;
  for(size_t i=0;i<model.meshes.size();++i) {const auto& value=model.meshes[i];if(!property(ResourceDomain::Other,value,location(9,i))) return false;for(size_t j=0;j<value.primitives.size();++j) if(!property(ResourceDomain::Primitive,value.primitives[j],location(10,i,j))) return false;}
  for(size_t i=0;i<model.skins.size();++i) if(!property(ResourceDomain::Other,model.skins[i],location(11,i))) return false;
  for(size_t i=0;i<model.lights.size();++i) if(!property(ResourceDomain::Light,model.lights[i],location(12,i))) return false;
  for(size_t i=0;i<model.images.size();++i) if(!property(ResourceDomain::Other,model.images[i],location(13,i))) return false;
  for(size_t i=0;i<model.samplers.size();++i) if(!property(ResourceDomain::Other,model.samplers[i],location(14,i))) return false;
  for(size_t i=0;i<model.textures.size();++i) if(!property(ResourceDomain::Texture,model.textures[i],location(15,i))) return false;
  for(size_t i=0;i<model.materials.size();++i) {const auto& value=model.materials[i];
    if(!property(ResourceDomain::Material,value,location(16,i)) || !property(ResourceDomain::Other,value.pbrMetallicRoughness,location(17,i)) ||
        !property(ResourceDomain::TextureInfo,value.pbrMetallicRoughness.baseColorTexture,location(18,i)) ||
        !property(ResourceDomain::TextureInfo,value.pbrMetallicRoughness.metallicRoughnessTexture,location(19,i)) ||
        !property(ResourceDomain::TextureInfo,value.normalTexture,location(20,i)) || !property(ResourceDomain::TextureInfo,value.occlusionTexture,location(21,i)) || !property(ResourceDomain::TextureInfo,value.emissiveTexture,location(22,i))) return false;
  }
  for(size_t i=0;i<model.animations.size();++i) {const auto& value=model.animations[i];
    if(!property(ResourceDomain::Other,value,location(23,i))) return false;
    for(size_t j=0;j<value.samplers.size();++j) if(!property(ResourceDomain::Other,value.samplers[j],location(24,i,j))) return false;
    for(size_t j=0;j<value.channels.size();++j) {const auto& channel=value.channels[j];
      if(!property(ResourceDomain::Other,channel,location(25,i,j)) || !visitor(ResourceDomain::Target,channel.target_extensions,std::string_view(channel.target_extensions_json_string),location(26,i,j))) return false;
    }
  }
  return true;
}
struct ResourceUnknownStep {std::string_view key;size_t index{};bool array{};};
std::string resourceUnknownPath(std::span<const ResourceUnknownStep> path,ResourceBudget& budget) {
  uint64_t capacity{};for(const auto& part:path) {
    const uint64_t length=part.array?24:part.key.size()*2+1;
    if(length>UINT64_MAX-capacity) throw std::length_error("Material unknown-extension path overflows");capacity+=length;
  }
  budget.add(capacity);std::string result;result.reserve(size_t(capacity));
  for(const auto& part:path) {
    result.push_back('/');if(part.array) {result+=std::to_string(part.index);continue;}
    for(const char letter:part.key) {if(letter=='~') result+="~0";else if(letter=='/') result+="~1";else result.push_back(letter);}
  }
  return result;
}
template<class Visitor> void auditResourceNested(const tinygltf::Value& value,ResourceBudget& budget,Visitor&& unknown) {
  std::array<ResourceUnknownStep,128> path;
  const auto walk=[&](auto&& self,const tinygltf::Value& item,size_t level)->void {
    if(level>budget.depth || level>=path.size() || !budget.values) throw std::length_error("Material resource reference audit exceeds its depth/work limit");--budget.values;
    if(item.IsArray()) {size_t index{};for(const auto& child:item.Get<tinygltf::Value::Array>()) {path[level]={{},index++,true};self(self,child,level+1);}}
    else if(item.IsObject()) for(const auto& [name,child]:item.Get<tinygltf::Value::Object>()) {
      if(name=="extras") continue; // Application metadata is not a normative index space.
      path[level]={name,0,false};
      if(name!="extensions") {self(self,child,level+1);continue;}
      if(!child.IsObject()) throw std::invalid_argument("Nested normative extensions must be an object");
      if(level+1>=path.size()) throw std::length_error("Material unknown-extension path exceeds its depth limit");
      for(const auto& [extension,body]:child.Get<tinygltf::Value::Object>()) {
        path[level+1]={extension,0,false};
        if(resourceExtensionAllowed(extension,ResourceDomain::TextureInfo) || resourceExtensionAllowed(extension,ResourceDomain::Light)) self(self,body,level+2);
        else unknown(std::span<const ResourceUnknownStep>(path.data(),level+2),body);
      }
    }
  };
  walk(walk,value,0);
}
void auditResourcePointer(const tinygltf::Model& model,const tinygltf::Value& body,size_t textures,size_t images,size_t samplers) {
  if(!body.IsObject()) throw std::invalid_argument("Animation pointer extension must be an object");
  const tinygltf::Value* field=nullptr;
  for(const auto& [name,value]:body.Get<tinygltf::Value::Object>()) if(name=="pointer") {field=&value;break;}
  if(!field || !field->IsString()) throw std::invalid_argument("Animation pointer is missing a string path");
  const std::string_view path=field->Get<std::string>();
  for(const auto [root,count]:{std::pair<std::string_view,size_t>{"/textures/",textures},{"/images/",images},{"/samplers/",samplers}}) {
    if(!path.starts_with(root)) continue;
    const auto tail=path.substr(root.size());const auto end=tail.find('/');const auto index=tail.substr(0,end);
    uint64_t value{};const auto parsed=std::from_chars(index.data(),index.data()+index.size(),value);
    if(index.empty() || (index.size()>1 && index[0]=='0') || parsed.ec!=std::errc{} || parsed.ptr!=index.data()+index.size() || value>=count)
      throw std::invalid_argument("Animation pointer retains a removed texture/image/sampler resource");
    if(path.ends_with("/index") || path.ends_with("/source") || path.ends_with("/sampler"))
      throw std::invalid_argument("Animated resource-index fields require an explicit reference evaluator");
    throw std::invalid_argument("Resource animation pointer schema has no supported reference evaluator");
  }
  if(!container::geometry::gltf::IsSupportedGltfNonResourceAnimationPointer(model,path))
    throw std::invalid_argument("Unknown animation pointer schema may retain removed texture resources");
}
}

struct SceneManager::MaterialResourceCommand::State {
  const SceneManager* owner{};
  std::shared_ptr<const uint8_t> ownerIdentity,resourceOwner,cookie,childVacancy;
  uint64_t asset{},bytes{};
  int32_t material{-1};uint32_t selectedArchive{UINT32_MAX},selectedEvaluated{UINT32_MAX};
  enum class Phase:uint8_t {Unpublished,Present,Detached} phase{Phase::Unpublished};
  size_t aImages{},aSamplers{},aTextures{},eImages{},eSamplers{},eTextures{},runtime{},requests{};
  std::vector<tinygltf::Image> aImageTail,eImageTail;
  std::vector<tinygltf::Sampler> aSamplerTail,eSamplerTail;
  std::vector<tinygltf::Texture> aTextureTail,eTextureTail;
  std::vector<container::material::TextureResource> runtimeTail;
  std::vector<MaterialTextureSourceRequest> requestTail;
  std::vector<int32_t> mapTail;
  std::vector<std::shared_ptr<const uint8_t>> identityTail;
  ResourceImageOwnership imageOwnership;
  TextureResourceLifecycleCommand logicalCommand;
  container::material::RuntimeTextureAppendCommand runtimeAppend;
  TextureResourceReferenceOwnerPtr logicalReferences;
  container::util::ResourceHandle logicalMaterial;
  std::shared_ptr<const TextureSamplingFallback> fallback;
  struct Role {uint32_t archive{},evaluated{},beforeA{UINT32_MAX},beforeE{UINT32_MAX},after{UINT32_MAX};bool color{};};
  std::vector<Role> roles;
  struct Mapping {uint32_t archive{};int32_t before{-1},after{-1};};
  std::vector<Mapping> mappings;
  struct Identity {uint32_t archive{};std::shared_ptr<const uint8_t> before,after;};
  std::vector<Identity> identities;
  std::vector<std::string> aUsed,eUsed,aRequired,eRequired;
  std::vector<std::pair<uint32_t,tinygltf::Texture>> prefixATextures,prefixETextures;
  std::vector<std::pair<uint32_t,tinygltf::Sampler>> prefixASamplers,prefixESamplers;
  std::vector<std::pair<uint32_t,tinygltf::Image>> prefixAImages,prefixEImages;
  std::vector<std::pair<uint32_t,std::shared_ptr<const uint8_t>>> prefixIdentities;
  std::vector<std::pair<uint32_t,int32_t>> prefixMappings;
  struct UnknownExtension {uint64_t location{};uint8_t domain{};std::string name,path;tinygltf::Value body;};
  std::vector<UnknownExtension> unknownA,unknownE;
};

struct SceneManager::PreparedMaterialResources::State {
  struct Tables {
    std::vector<tinygltf::Image> aImages,eImages;
    std::vector<tinygltf::Sampler> aSamplers,eSamplers;
    std::vector<tinygltf::Texture> aTextures,eTextures;
    std::vector<std::string> aUsed,eUsed,aRequired,eRequired;
    container::material::GltfTextureResources aRoles,eRoles;
    std::vector<int32_t> mapping,exportIndices;
    std::vector<std::shared_ptr<const uint8_t>> identities;
    std::vector<MaterialTextureSourceRequest> requests;
    container::material::TextureManager textures;
    ResourceImageOwnership imageOwnership;
    TextureResourceLifecycleGenerationPtr logicalGeneration;
    SourceResourcePhaseTable aPhases,ePhases;
  } before,next;
  const SceneManager* owner{};
  std::shared_ptr<const uint8_t> ownerIdentity,resourceOwner;
  std::shared_ptr<const uint8_t> expectedVacancy,futureVacancy;
  std::shared_ptr<container::gpu::VulkanDevice> device;
  container::gpu::AllocationManager* allocation{};
  uint64_t epoch{},materialRevision{},authoredRevision{},geometryRevision{},asset{},resourceRevision{};
  uint64_t samplingRevisionBefore{};
  size_t materialCount{},sourceCount{},beforeMetadataCount{};
  size_t samplingRowsBefore{};
  uint32_t textureCapacity{},samplerCapacity{},selectedArchive{UINT32_MAX},selectedEvaluated{UINT32_MAX};
  int32_t sourceMaterial{-1};
  std::array<uint32_t,2> inputArchives{UINT32_MAX,UINT32_MAX};
  VkDescriptorPool pool{};VkDescriptorSetLayout layout{};
  VkBuffer materialBuffer{},metadataBefore{};
  std::shared_ptr<const uint8_t> clipFrames;
  VmaAllocation metadataAllocationBefore{};
  MaterialResourcesLimits limits;
  ResourceBudget budget;
  uint64_t newTextureBytes{};
  std::vector<VkSampler> expectedSamplers,samplers,createdSamplers;
  container::material::MaterialSamplerSlotPlan slots;
  std::vector<VkSampler> descriptorSamplers;
  std::vector<VkDescriptorImageInfo> images;
  std::vector<container::gpu::GpuTextureMetadata> metadata;
  container::material::TextureSamplingPlan sampling;
  std::shared_ptr<const TextureSamplingFallback> fallback;
  // A committed upload batch is a transient rollback owner only. It is cleared
  // after complete candidate and selected history pins exist, before ready.
  std::shared_ptr<const container::gpu::AllocationManager::TextureAllocationLease> stagedBatch;
  std::vector<tinygltf::Material> referenceAuthored,referenceEvaluated;
  std::vector<container::geometry::gltf::ImportedMaterialTextureBindings> referenceRuntime;
  struct ExtensionWitness {uint8_t domain{};tinygltf::ExtensionMap fields;std::string raw;};
  std::vector<ExtensionWitness> referenceExtensionsA,referenceExtensionsE;
  std::vector<RuntimeMaterialProvenance> candidateProvenance;
  container::gpu::AllocatedBuffer metadataBuffer;
  MaterialResourceCommand command;
  PreparedTextureResourceLifecycle logicalChange;
  std::optional<container::material::PreparedRuntimeTextureAppend> runtimeAppend;
  uint64_t logicalBytesAccounted{};
  TextureResourceReferenceOwnerPtr beforeLogicalReaders,futureLogicalPreparation;
  std::vector<MaterialResourceDependencyStatePtr> standaloneMaterialDependencies;
  container::util::ResourceHandle pairedMaterial;
  bool pairedMaterialPublication{},pairedMaterialStaged{};
  [[nodiscard]] TextureResourceLifecycleView nextLifecycleView() const noexcept {
    return {{next.aImages,next.aTextures,next.aSamplers,owner->exportSource_.bufferViews,owner->exportSource_.buffers,
            next.aRoles.color,next.aRoles.data,true,next.aPhases.images,next.aPhases.textures,next.aPhases.samplers},
        {next.eImages,next.eTextures,next.eSamplers,owner->gltfModel_.bufferViews,owner->gltfModel_.buffers,
            next.eRoles.color,next.eRoles.data,true,next.ePhases.images,next.ePhases.textures,next.ePhases.samplers},next.textures,next.mapping,next.identities};
  }
  bool ready{},consumed{},replay{},forward{true};
  ~State() {
    if(metadataBuffer.buffer!=VK_NULL_HANDLE) allocation->destroyBuffer(metadataBuffer);
    if(device) for(auto sampler:createdSamplers) if(sampler!=VK_NULL_HANDLE)
      destroyOwnedSampler(device->device(),sampler,nullptr);
  }
};

SceneManager::MaterialResourceCommand::operator bool() const noexcept {return bool(state_);}
int32_t SceneManager::MaterialResourceCommand::sourceMaterial() const noexcept {return state_?state_->material:-1;}
bool SceneManager::MaterialResourceCommand::published() const noexcept {return state_ && state_->phase!=State::Phase::Unpublished;}
bool SceneManager::MaterialResourceCommand::present() const noexcept {return state_ && state_->phase==State::Phase::Present;}
uint64_t SceneManager::MaterialResourceCommand::ownedBytes() const noexcept {return state_?state_->bytes:0;}
ResourceImageOwnership SceneManager::MaterialResourceCommand::textureImageOwnership() const noexcept {return state_?state_->imageOwnership:ResourceImageOwnership{};}
TextureResourceReferenceOwnerPtr SceneManager::MaterialResourceCommand::resourceReferences() const noexcept {return state_?state_->logicalReferences:TextureResourceReferenceOwnerPtr{};}
SceneManager::PreparedMaterialResources::PreparedMaterialResources() noexcept=default;
SceneManager::PreparedMaterialResources::~PreparedMaterialResources()=default;
SceneManager::PreparedMaterialResources::PreparedMaterialResources(PreparedMaterialResources&&) noexcept=default;
SceneManager::PreparedMaterialResources& SceneManager::PreparedMaterialResources::operator=(PreparedMaterialResources&&) noexcept=default;
SceneManager::PreparedMaterialResources::operator bool() const noexcept {return state_ && !state_->consumed;}
uint32_t SceneManager::PreparedMaterialResources::sourceTexture() const noexcept {return state_?state_->selectedArchive:UINT32_MAX;}
uint32_t SceneManager::PreparedMaterialResources::evaluatedTexture() const noexcept {return state_?state_->selectedEvaluated:UINT32_MAX;}
uint64_t SceneManager::PreparedMaterialResources::ownedBytes() const noexcept {
  if(!state_) return 0;
  const auto base=state_->limits.maxPreparedCpuBytes-state_->budget.remaining;
  const auto logical=state_->logicalChange.ownedBytes();
  const auto pending=logical>state_->logicalBytesAccounted?logical-state_->logicalBytesAccounted:0;
  return pending>UINT64_MAX-base?UINT64_MAX:base+pending;
}
SceneManager::MaterialResourceCommand SceneManager::PreparedMaterialResources::command() const noexcept {return state_?state_->command:MaterialResourceCommand{};}
TextureResourceLifecycleGenerationPtr SceneManager::PreparedMaterialResources::resourceLifecycleGeneration() const noexcept {
  return state_?state_->next.logicalGeneration:TextureResourceLifecycleGenerationPtr{};
}
std::span<const int32_t> SceneManager::PreparedMaterialResources::exportTextureIndices() const noexcept {
  return state_?std::span<const int32_t>(state_->next.exportIndices):std::span<const int32_t>{};
}
TextureResourceReferenceOwnerPtr SceneManager::PreparedMaterialResources::retainResourceReferences(
    std::span<const TextureResourceSelection> selected,container::util::ResourcePinClass pinClass,uint64_t maxBytes,bool opaque) const {
  if(!*this || !state_->logicalChange) throw std::invalid_argument("Prepared resource logical identities are unavailable");
  return state_->logicalChange.select(selected,pinClass,maxBytes,opaque);
}
TextureResourceReferenceOwnerPtr SceneManager::PreparedMaterialResources::retainAllResourceReferences(
    container::util::ResourcePinClass pinClass,uint64_t maxBytes) const {
  if(!*this || !state_->logicalChange) throw std::invalid_argument("Prepared resource logical identities are unavailable");
  return state_->logicalChange.selectAll(pinClass,maxBytes);
}
TextureResourceLiveOwnerPtr SceneManager::PreparedMaterialResources::stageLiveOwner(TextureResourceLiveOwnerKey key,
    std::span<const TextureResourceSelection> selected,const TextureResourceLiveOwnerPtr& outgoing,uint64_t maxBytes,bool opaque) {
  if(!*this || !state_->ready || !state_->owner->canPrepareMaterialResources(*this))
    throw std::invalid_argument("Prepared resources are not structurally current for live dependencies");
  if(key.kind==TextureResourceLiveOwnerKind::Material &&
      (!state_->pairedMaterialPublication || key.owner!=state_->pairedMaterial))
    throw std::invalid_argument("Material dependency publication requires its validated paired material plan");
  auto owner=state_->logicalChange.stageLiveOwner(key,selected,outgoing,maxBytes,opaque);
  if(state_->pairedMaterialPublication && key.kind==TextureResourceLiveOwnerKind::Material && key.owner==state_->pairedMaterial)
    state_->pairedMaterialStaged=true;
  return owner;
}
void SceneManager::PreparedMaterialResources::releaseLiveOwners(std::span<const TextureResourceLiveOwnerPtr> outgoing) {
  if(!*this || !state_->ready || !state_->owner->canPrepareMaterialResources(*this))
    throw std::invalid_argument("Prepared resources are not structurally current for dependency release");
  for(const auto& owner:outgoing) if(owner && owner->key().kind==TextureResourceLiveOwnerKind::Material &&
      (!state_->pairedMaterialPublication || owner->key().owner!=state_->pairedMaterial))
    throw std::invalid_argument("Material dependency release requires its validated paired material plan");
  state_->logicalChange.releaseLiveOwners(outgoing);
}
bool SceneManager::PreparedMaterialResources::liveOwnerWillBeActive(const TextureResourceLiveOwnerPtr& owner) const noexcept {
  return state_ && state_->ready && !state_->consumed && state_->logicalChange.liveOwnerWillBeActive(owner);
}
void SceneManager::PreparedMaterialResources::sealLiveDependencies() {
  if(!*this || !state_->ready || !state_->owner->canPrepareMaterialResources(*this))
    throw std::invalid_argument("Prepared resources are not structurally current for publication");
  if(state_->logicalChange.sealed()) return;
  if(state_->pairedMaterialPublication) {
    if(!state_->pairedMaterialStaged) throw std::invalid_argument("Paired resource publication lacks its validated material successor");
  } else {
    // A standalone resource operation cannot release a material definition.
    // Preflight every owner before staging any replacement, so a bound-tail
    // candidate remains available for an explicitly validated paired plan.
    for(uint32_t row=0;row<state_->owner->materialManager_.materialCount();++row) {
      const auto owner=state_->owner->materialResourceLiveOwner(row);
      if(owner && !owner->matches(state_->next.logicalGeneration))
        throw std::invalid_argument("Resource-only publication would remove a live material dependency");
    }
    state_->standaloneMaterialDependencies=state_->owner->stageCurrentMaterialResourceDependencies(
        state_->logicalChange,state_->budget.remaining,{},nullptr,state_->candidateProvenance);
    state_->budget.add(SceneManager::materialResourceDependencyMapBytes(
        state_->standaloneMaterialDependencies,state_->owner->materialResourceDependencies_));
  }
  const auto logicalBytes=state_->logicalChange.ownedBytes();
  if(logicalBytes>state_->logicalBytesAccounted) state_->budget.add(logicalBytes-state_->logicalBytesAccounted);
  state_->logicalBytesAccounted=logicalBytes;
  state_->logicalChange.seal();
}

std::vector<SceneManager::MaterialResourceDependencyStatePtr> SceneManager::stageResourceMaterialDependencies(
    PreparedMaterialResources& resources,uint32_t runtime,container::util::ResourceHandle material,
    const container::geometry::gltf::AnimationPlayer* player,uint64_t maxBytes) const {
  if(!canPrepareMaterialResources(resources) || resources.state_->logicalChange.sealed() || !material ||
      material.kind()!=container::util::ResourceKind::Material || material.domain()!=materialLifecycleDomain_)
    throw std::invalid_argument("Paired resource dependency staging has a stale material or sealed candidate");
  auto& state=*resources.state_;
  if(state.pairedMaterialPublication) throw std::invalid_argument("A resource candidate already belongs to a material plan");
  state.pairedMaterialPublication=true;state.pairedMaterial=std::move(material);
  return stageCurrentMaterialResourceDependencies(state.logicalChange,maxBytes,{},player,state.candidateProvenance,runtime);
}
const container::material::TextureManager& SceneManager::PreparedMaterialResources::textures() const {
  if(!*this) throw std::invalid_argument("Material resources are empty or consumed");return state_->next.textures;
}
const container::gpu::AllocatedBuffer& SceneManager::PreparedMaterialResources::metadataBuffer() const {
  if(!*this || !state_->ready) throw std::invalid_argument("Material resources are not prepared");return state_->metadataBuffer;
}
std::span<const VkDescriptorImageInfo> SceneManager::PreparedMaterialResources::imageInfos() const noexcept {return state_?std::span<const VkDescriptorImageInfo>(state_->images):std::span<const VkDescriptorImageInfo>{};}
std::span<const VkSampler> SceneManager::PreparedMaterialResources::descriptorSamplers() const noexcept {return state_?std::span<const VkSampler>(state_->descriptorSamplers):std::span<const VkSampler>{};}
ResourceImageOwnership SceneManager::PreparedMaterialResources::textureImageOwnership() const noexcept {return state_?state_->next.imageOwnership:ResourceImageOwnership{};}
container::geometry::gltf::GltfMaterialAuthoringSourceView SceneManager::PreparedMaterialResources::authoredSourceView() const noexcept {
  if(!state_) return {};return {state_->sourceCount,state_->next.aTextures.size(),state_->next.aUsed,state_->owner->exportSource_.animations};
}
container::geometry::gltf::GltfMaterialAuthoringSourceView SceneManager::PreparedMaterialResources::evaluatedSourceView() const noexcept {
  if(!state_) return {};return {state_->sourceCount,state_->next.eTextures.size(),state_->next.eUsed,state_->owner->gltfModel_.animations};
}
container::material::GltfMaterialConversionContext SceneManager::PreparedMaterialResources::conversionContext() const noexcept {
  if(!state_) return {};return {state_->next.eTextures.size(),state_->next.eUsed,state_->next.eRoles.color,state_->next.eRoles.data,state_->next.textures.textureCount()};
}
SceneManager::MaterialTextureReference SceneManager::PreparedMaterialResources::reference(bool color) const {return reference(sourceTexture(),color);}
SceneManager::MaterialTextureReference SceneManager::PreparedMaterialResources::reference(uint32_t archive,bool color) const {
  if(!*this) throw std::invalid_argument("Material resources are empty or consumed");const auto& s=*state_;const auto& t=s.next;
  const auto& roles=color?t.aRoles.color:t.aRoles.data;
  if(archive>=roles.size() || archive>=t.mapping.size() || archive>=t.identities.size())
    throw std::invalid_argument("Prospective material texture source is unavailable");
  MaterialTextureReference result;result.ownerIdentity_=s.resourceOwner;result.sourceIdentity_=t.identities[archive];
  result.archive_=archive;result.runtime_=roles[archive];result.color_=color;
  if(t.mapping[archive]>=0) result.evaluated_=uint32_t(t.mapping[archive]);
  if(!matchesReference(result,color,result.runtime_)) throw std::invalid_argument("Prospective material texture role is unavailable");
  return result;
}
bool SceneManager::PreparedMaterialResources::matchesReference(const MaterialTextureReference& r,bool color,uint32_t runtime) const noexcept {
  if(!*this || !r) return false;const auto& s=*state_;const auto& t=s.next;
  if(r.ownerIdentity_!=s.resourceOwner || r.color_!=color || r.runtime_!=runtime || r.archive_>=t.aTextures.size() ||
      r.archive_>=t.identities.size() || r.sourceIdentity_!=t.identities[r.archive_] || r.archive_>=t.mapping.size() ||
      !t.textures.isTextureActive(r.runtime_)) return false;
  const auto active=[](std::span<const uint8_t> mask,size_t count,size_t row) noexcept {
    return row<count && (mask.empty() || (mask.size()==count && mask[row]==1));
  };
  if(!active(t.aPhases.textures,t.aTextures.size(),r.archive_)) return false;
  const auto& a=color?t.aRoles.color:t.aRoles.data;const auto& e=color?t.eRoles.color:t.eRoles.data;
  const auto mapped=t.mapping[r.archive_];
  if(r.archive_>=a.size() || a[r.archive_]!=runtime) return false;
  if(mapped<0) return r.evaluated_==UINT32_MAX;
  if(!active(t.ePhases.textures,t.eTextures.size(),size_t(mapped))) return false;
  return r.evaluated_==uint32_t(mapped) && size_t(mapped)<t.eTextures.size() && size_t(mapped)<e.size() && e[size_t(mapped)]==runtime;
}
SceneManager::MaterialTextureProvenance SceneManager::PreparedMaterialResources::makeProvenance(
    const container::material::Material& material,const tinygltf::Material& authored,const tinygltf::Material& evaluated) const {
  if(!*this) throw std::invalid_argument("Material resources are empty or consumed");
  const auto a=container::material::ReadGltfMaterialSourceTextures(authored,state_->next.aTextures.size());
  const auto e=container::material::ReadGltfMaterialSourceTextures(evaluated,state_->next.eTextures.size());
  const auto bindings=container::geometry::gltf::snapshotMaterialTextureBindings(material).runtimeIndices;
  MaterialTextureProvenance result;
  for(size_t i=0;i<bindings.size();++i) {
    if(bindings[i]==UINT32_MAX) {if(a[i]>=0 || e[i]>=0) throw std::invalid_argument("Prospective material source has no resident texture role");continue;}
    if(a[i]<0 || e[i]<0) throw std::invalid_argument("Prospective material has no explicit texture source identity");
    auto r=reference(uint32_t(a[i]),colorSlot(i,material));
    if(r.evaluatedTexture()!=std::optional<uint32_t>(uint32_t(e[i])) || !matchesReference(r,colorSlot(i,material),bindings[i]))
      throw std::invalid_argument("Prospective authored/evaluated material texture roles disagree");
    result[i]=std::move(r);
  }
  return result;
}
SceneManager::MaterialTextureProvenance SceneManager::PreparedMaterialResources::forwardProvenance(
    const MaterialTextureProvenance& row,const container::material::Material& material) const {
  if(!*this) throw std::invalid_argument("Material resources are empty or consumed");
  const auto& s=*state_;const auto& before=s.before;
  const auto actual=container::geometry::gltf::snapshotMaterialTextureBindings(material).runtimeIndices;
  MaterialTextureProvenance result;
  for(size_t slot=0;slot<actual.size();++slot) {
    if(actual[slot]==UINT32_MAX) {if(row[slot]) throw std::invalid_argument("Absent texture role has stale provenance");continue;}
    if(!row[slot]) throw std::invalid_argument("Texture role has no provenance to forward");
    const auto& old=*row[slot];const auto& roles=colorSlot(slot,material)?before.aRoles.color:before.aRoles.data;
    if(old.ownerIdentity_!=s.resourceOwner || old.archive_>=roles.size() || old.archive_>=before.identities.size() ||
        old.archive_>=before.mapping.size() || old.sourceIdentity_!=before.identities[old.archive_] ||
        old.runtime_!=actual[slot] || old.color_!=colorSlot(slot,material) || roles[old.archive_]!=actual[slot] ||
        (before.mapping[old.archive_]>=0 && old.evaluated_!=uint32_t(before.mapping[old.archive_])))
      throw std::invalid_argument("Texture provenance changed before its resource mapping was forwarded");
    const auto mapped=before.mapping[old.archive_];const auto& evaluatedRoles=colorSlot(slot,material)?before.eRoles.color:before.eRoles.data;
    if(mapped<0 ? old.evaluated_!=UINT32_MAX : old.evaluated_!=uint32_t(mapped) || size_t(mapped)>=evaluatedRoles.size() || evaluatedRoles[size_t(mapped)]!=actual[slot])
      throw std::invalid_argument("Texture provenance has an invalid previous evaluated role");
    auto next=reference(old.archive_,colorSlot(slot,material));
    if(next.runtimeResource()!=actual[slot]) throw std::invalid_argument("Resource command would invalidate a retained material texture role");
    result[slot]=std::move(next);
  }
  return result;
}
bool SceneManager::PreparedMaterialResources::provenanceMatches(const MaterialTextureProvenance& row,const container::material::Material& material) const noexcept {
  const auto bindings=container::geometry::gltf::snapshotMaterialTextureBindings(material).runtimeIndices;
  for(size_t i=0;i<bindings.size();++i) {
    if(bindings[i]==UINT32_MAX) {if(row[i]) return false;}
    else if(!row[i] || !matchesReference(*row[i],colorSlot(i,material),bindings[i])) return false;
  }
  return true;
}

SceneManager::PreparedMaterialResources SceneManager::prepareMaterialResourcesBase(int32_t source,const MaterialResourcesLimits& limits,bool resourceOnly) const {
  validateResourceLimits(limits);(void)authoredMaterial(source);
  if(!resourceOnly && !authoredMaterialIsActive(source)) throw std::invalid_argument("A retired material cannot prepare texture resources");
  if(sourceMaterialToRuntime_.size()!=gltfModel_.materials.size() || size_t(source)>=sourceMaterialToRuntime_.size() ||
      sourceMaterialToRuntime_[size_t(source)]>=materialCount())
    throw std::invalid_argument("Material resource source/runtime mapping is unavailable");
  if(!allocationManager_ || !deviceWrapper_ || materialResourceRevision_==UINT64_MAX ||
      descriptorSetLayout_==VK_NULL_HANDLE || textureMetadataBuffer_.buffer==VK_NULL_HANDLE ||
      materialBuffer_.buffer==VK_NULL_HANDLE || sceneClipBuffers().empty() ||
      materialSamplers_.size()!=container::material::kMaterialSamplerKeyCount)
    throw std::invalid_argument("Live material resource state is unavailable");
  // An immediate producer can append CPU resources before it requests a fresh
  // descriptor generation. Only an unchanged uploaded prefix is accepted here;
  // the candidate below prepares complete coherent metadata for every CPU row.
  if(!textureSamplingStateValidForResources())
    throw std::invalid_argument("Live material sampling plan or private fallback is unavailable");
  if(exportSource_.textures.size()!=archiveTextureResources_.color.size() || exportSource_.textures.size()!=archiveTextureResources_.data.size() ||
      exportSource_.textures.size()!=archiveTextureToEvaluated_.size() || exportSource_.textures.size()!=archiveTextureIdentities_.size() ||
      gltfModel_.textures.size()!=gltfTextureResources_.color.size() || gltfModel_.textures.size()!=gltfTextureResources_.data.size())
    throw std::invalid_argument("Material resource identity tables disagree");
  ResourceBudget budget{limits.maxPreparedCpuBytes,limits.maxMetadataValues,limits.maxMetadataDepth};
  budget.add(sizeof(PreparedMaterialResources::State)+sizeof(MaterialResourceCommand::State));
  const auto rows=[&](const auto& table) {
    if(table.size()>limits.maxSourceCount) throw std::length_error("Material resource source count exceeds its limit");
    for(const auto& row:table) budget.record(row,2);
  };
  rows(exportSource_.images);rows(gltfModel_.images);rows(exportSource_.samplers);rows(gltfModel_.samplers);
  rows(exportSource_.textures);rows(gltfModel_.textures);
  for(const auto* names:{&exportSource_.extensionsUsed,&gltfModel_.extensionsUsed,&exportSource_.extensionsRequired,&gltfModel_.extensionsRequired}) {
    if(names->size()>limits.maxMetadataValues) throw std::length_error("Material resource declarations exceed their limit");
    for(const auto& name:*names) budget.add(name.size()+sizeof(std::string),2);
  }
  if(textureManager_.textureCount()>textureDescriptorCapacity_ || textureManager_.textureCount()>limits.maxSourceCount ||
      materialTextureSourceRequests_.size()>limits.maxSourceCount)
    throw std::length_error("Material resource runtime/cache count exceeds its capacity");
  for(uint32_t i=0;i<textureManager_.textureCount();++i) {
    const auto* row=textureManager_.getTexture(i);
    if(row && row->imageView==VK_NULL_HANDLE) throw std::invalid_argument("Material resource contains an unavailable active image view");
    budget.add(container::material::TextureManager::rowStorageBytes()+128,3);
    if(row) budget.add(row->name.size()+row->contentKey.size(),3);
  }
  for(const auto& request:materialTextureSourceRequests_) budget.add(sizeof(request)+request.key.size()+64,2);
  budget.add(exportSource_.textures.size()+gltfModel_.textures.size(),sizeof(uint32_t)*4+sizeof(std::shared_ptr<const uint8_t>)*2);
  budget.add(exportTextureIndices_.size(),sizeof(int32_t)*2);
  budget.add(sizeof(ResourceImageOwnership),2);
  if(!container::material::TextureImageOwnershipMatches(textureImageOwnership_,textureManager_,*allocationManager_))
    throw std::invalid_argument("Material resource live image ownership is incomplete");
  budget.add(materialSamplers_.size(),sizeof(VkSampler)*4);
  PreparedMaterialResources result;result.state_=std::make_unique<PreparedMaterialResources::State>();auto& s=*result.state_;
  s.owner=this;s.ownerIdentity=sceneRootOwnerIdentity_;s.resourceOwner=materialResourceOwnerIdentity_;s.device=deviceWrapper_;s.allocation=allocationManager_;
  s.fallback=textureSamplingFallback_;
  s.epoch=generationEpoch_;s.materialRevision=materialRevision_;s.authoredRevision=authoredMaterialRevision_;s.geometryRevision=geometryRevision_;
  s.asset=materialAssetIdentity();s.resourceRevision=materialResourceRevision_;s.materialCount=materialCount();s.sourceCount=gltfModel_.materials.size();s.sourceMaterial=source;
  s.expectedVacancy=materialResourceVacancy_;
  s.textureCapacity=textureDescriptorCapacity_;s.samplerCapacity=materialSamplerDescriptorCapacity_;s.pool=descriptorPool_;s.layout=descriptorSetLayout_;
  s.materialBuffer=materialBuffer_.buffer;s.metadataBefore=textureMetadataBuffer_.buffer;s.metadataAllocationBefore=textureMetadataBuffer_.allocation;
  s.beforeMetadataCount=textureMetadata_.size();s.clipFrames=sceneClipFrameIdentity();s.limits=limits;s.budget=budget;
  s.samplingRevisionBefore=textureSamplingPlan_.textureRevision;s.samplingRowsBefore=textureSamplingPlan_.rows.size();
  auto& b=s.before;
  b.aImages=exportSource_.images;b.eImages=gltfModel_.images;b.aSamplers=exportSource_.samplers;b.eSamplers=gltfModel_.samplers;
  b.aTextures=exportSource_.textures;b.eTextures=gltfModel_.textures;b.aUsed=exportSource_.extensionsUsed;b.eUsed=gltfModel_.extensionsUsed;
  b.aRequired=exportSource_.extensionsRequired;b.eRequired=gltfModel_.extensionsRequired;
  b.aRoles=archiveTextureResources_;b.eRoles=gltfTextureResources_;b.mapping=archiveTextureToEvaluated_;b.identities=archiveTextureIdentities_;
  b.requests=materialTextureSourceRequests_;b.exportIndices=exportTextureIndices_;b.textures=textureManager_;b.imageOwnership=textureImageOwnership_;
  refreshTextureResourceLifecycle(container::util::ResourceRetirementPolicy::InvalidatePreparations,s.budget.remaining);
  s.budget.add(archiveResourcePhases_.images.size()+archiveResourcePhases_.textures.size()+archiveResourcePhases_.samplers.size()+
      gltfResourcePhases_.images.size()+gltfResourcePhases_.textures.size()+gltfResourcePhases_.samplers.size(),2);
  b.aPhases=archiveResourcePhases_;b.ePhases=gltfResourcePhases_;
  b.logicalGeneration=textureResourceLifecycle_;
  if(!b.logicalGeneration || !b.logicalGeneration->matches(textureResourceLifecycleView()))
    throw std::invalid_argument("Material resource logical identities are stale or incomplete");
  s.beforeLogicalReaders=RetainAllTextureResourceReferences(b.logicalGeneration,container::util::ResourcePinClass::GpuReader,s.budget.remaining);
  s.budget.add(s.beforeLogicalReaders->ownedBytes());
  s.next=b;s.expectedSamplers=materialSamplers_;s.samplers=materialSamplers_;
  return result;
}

SceneManager::PreparedMaterialResources SceneManager::prepareMaterialTextureFile(int32_t source,const std::filesystem::path& path,
    MaterialTextureRoles roles,uint32_t sampler,const MaterialResourcesLimits& limits) const {
  const auto policy=container::material::DecodeMaterialSamplerKey({sampler});
  return prepareMaterialTextureFileWithAuthoredSampler(source,path,roles,
      container::material::MaterialTextureSamplerDescription{policy.minFilter,policy.magFilter,policy.wrapS,policy.wrapT},limits);
}

SceneManager::PreparedMaterialResources SceneManager::prepareMaterialTextureFileWithAuthoredSampler(int32_t source,const std::filesystem::path& path,
    MaterialTextureRoles roles,const container::material::MaterialTextureSamplerDescription& sampler,const MaterialResourcesLimits& limits) const {
  validateResourceLimits(limits);container::material::ValidateMaterialTextureSampler(sampler);
  if(uint8_t(roles)<1 || uint8_t(roles)>3) throw std::invalid_argument("Material texture roles are invalid");
  auto result=prepareMaterialResourcesBase(source,limits);
  const auto normalized=path.lexically_normal();const auto name=container::util::pathToUtf8(normalized);
  if(name.empty() || name.size()>65536 || name.find('\0')!=std::string::npos) throw std::invalid_argument("Material texture file path must be bounded and nonempty");
  std::ifstream stream(normalized,std::ios::binary|std::ios::ate);
  if(!stream) throw std::runtime_error("Failed to open material texture: "+name);
  const auto size=stream.tellg();if(size<=0 || uint64_t(size)>limits.maxEncodedBytes || uint64_t(size)>result.state_->budget.remaining/2)
    throw std::length_error("Material texture encoded bytes exceed their preparation capacity");
  std::vector<std::byte> bytes(static_cast<size_t>(size));stream.seekg(0);
  if(!stream.read(reinterpret_cast<char*>(bytes.data()),std::streamsize(bytes.size()))) throw std::runtime_error("Failed to read material texture: "+name);
  result.prepareSource({},"file:"+name,name,bytes,roles,std::nullopt,&sampler);result.finishPreparation();return result;
}
SceneManager::PreparedMaterialResources SceneManager::prepareMaterialTextureBytes(int32_t source,const std::string& name,std::span<const std::byte> bytes,
    MaterialTextureRoles roles,uint32_t sampler,const MaterialResourcesLimits& limits) const {
  validateResourceLimits(limits);(void)container::material::DecodeMaterialSamplerKey({sampler});
  if(uint8_t(roles)<1 || uint8_t(roles)>3) throw std::invalid_argument("Material texture roles are invalid");
  if(name.empty() || name.size()>65536 || name.find('\0')!=std::string::npos) throw std::invalid_argument("Material texture name must be bounded and nonempty");
  if(bytes.empty() || bytes.size()>limits.maxEncodedBytes) throw std::length_error("Material texture encoded bytes exceed their capacity");
  auto result=prepareMaterialResourcesBase(source,limits);result.prepareSource({},"bytes:"+name,name,bytes,roles,sampler);result.finishPreparation();return result;
}
SceneManager::PreparedMaterialResources SceneManager::prepareMaterialTextureSource(int32_t source,uint32_t texture,
    MaterialTextureRoles roles,std::optional<uint32_t> sampler,const MaterialResourcesLimits& limits) const {
  return prepareMaterialTextureSource(source,texture,roles,sampler,nullptr,limits);
}

SceneManager::PreparedMaterialResources SceneManager::prepareMaterialTextureSourceWithAuthoredSampler(int32_t source,uint32_t texture,
    MaterialTextureRoles roles,const container::material::MaterialTextureSamplerDescription& sampler,const MaterialResourcesLimits& limits) const {
  return prepareMaterialTextureSource(source,texture,roles,std::nullopt,&sampler,limits);
}

SceneManager::PreparedMaterialResources SceneManager::prepareMaterialTextureSource(int32_t source,uint32_t texture,
    MaterialTextureRoles roles,std::optional<uint32_t> sampler,
    const container::material::MaterialTextureSamplerDescription* authoredSampler,const MaterialResourcesLimits& limits) const {
  validateResourceLimits(limits);
  if(uint8_t(roles)<1 || uint8_t(roles)>3) throw std::invalid_argument("Material texture roles are invalid");
  if(sampler) (void)container::material::DecodeMaterialSamplerKey({*sampler});
  if(authoredSampler) container::material::ValidateMaterialTextureSampler(*authoredSampler);
  if(texture>=exportSource_.textures.size()) throw std::invalid_argument("Material source texture is unavailable");
  if(!sourceResourceActive({container::util::ResourceDomainKind::Archive,container::util::ResourceKind::Texture,texture}))
    throw std::invalid_argument("A retired source texture requires its original restore receipt");
  auto result=prepareMaterialResourcesBase(source,limits);result.prepareSource(texture,{}, {},{},roles,sampler,authoredSampler);result.finishPreparation();return result;
}

SceneManager::PreparedMaterialResources SceneManager::prepareMaterialResourcesChange(const MaterialResourceCommand& command,bool forward,
    const MaterialResourcesLimits& limits) const {
  if(!command) throw std::invalid_argument("Material resource history command is empty");const auto& c=*command.state_;
  using Phase=MaterialResourceCommand::State::Phase;
  if(c.owner!=this || c.ownerIdentity!=sceneRootOwnerIdentity_ || c.resourceOwner!=materialResourceOwnerIdentity_ || c.asset!=materialAssetIdentity() ||
      c.fallback!=textureSamplingFallback_ || !c.fallback || !c.fallback->lease ||
      c.phase!=(forward?Phase::Detached:Phase::Present) || (forward && materialResourceVacancy_!=c.cookie))
    throw std::invalid_argument("Material resource command has a stale owner, phase or branch vacancy");
  if(c.material<0 || size_t(c.material)>=sourceMaterialToRuntime_.size() ||
      sourceMaterialToRuntime_[size_t(c.material)]>=materialLifecycleHandles_.size() ||
      materialLifecycleHandles_[sourceMaterialToRuntime_[size_t(c.material)]]!=c.logicalMaterial ||
      !c.logicalReferences || !c.logicalReferences->valid())
    throw std::invalid_argument("Material resource history has a stale selected material incarnation");
  if(!container::material::TextureImageOwnershipValid(c.imageOwnership,*allocationManager_))
    throw std::invalid_argument("Material resource history has stale image ownership");
  for(const auto& resource:c.runtimeTail)
    if(!container::material::TextureImageOwnershipContains(c.imageOwnership,resource,*allocationManager_))
      throw std::invalid_argument("Material resource history does not own its retained runtime tail image");
  const auto inputRecords=[&](const auto& witnesses,const auto& current,auto&& equal) {
    for(const auto& [index,expected]:witnesses)
      if(index>=current.size() || !equal(expected,current[index])) throw std::invalid_argument("Material resource command input source changed");
  };
  const auto textureSame=[](const auto& a,const auto& b) {
    return a.source==b.source && a.sampler==b.sampler && sameResourceExtensions(a.extensions,b.extensions);
  };
  const auto samplerSame=[](const auto& a,const auto& b) {return a.minFilter==b.minFilter && a.magFilter==b.magFilter && a.wrapS==b.wrapS && a.wrapT==b.wrapT;};
  const auto imageSame=[](const auto& a,const auto& b) {
    return a.as_is==b.as_is && a.width==b.width && a.height==b.height && a.component==b.component && a.bits==b.bits &&
        a.pixel_type==b.pixel_type && a.image==b.image;
  };
  inputRecords(c.prefixATextures,exportSource_.textures,textureSame);inputRecords(c.prefixETextures,gltfModel_.textures,textureSame);
  inputRecords(c.prefixASamplers,exportSource_.samplers,samplerSame);inputRecords(c.prefixESamplers,gltfModel_.samplers,samplerSame);
  inputRecords(c.prefixAImages,exportSource_.images,imageSame);inputRecords(c.prefixEImages,gltfModel_.images,imageSame);
  for(const auto& [index,identity]:c.prefixIdentities)
    if(index>=archiveTextureIdentities_.size() || archiveTextureIdentities_[index]!=identity)
      throw std::invalid_argument("Material resource command input identity was replaced");
  for(const auto& [index,mapping]:c.prefixMappings) {
    auto expected=mapping;
    for(const auto& change:c.mappings) if(change.archive==index) expected=forward?change.before:change.after;
    if(index>=archiveTextureToEvaluated_.size() || archiveTextureToEvaluated_[index]!=expected)
      throw std::invalid_argument("Material resource command input mapping was replaced");
  }
  auto result=prepareMaterialResourcesBase(c.material,limits);auto& s=*result.state_;auto& t=s.next;
  s.replay=true;s.forward=forward;s.command=command;s.futureVacancy=forward?c.childVacancy:c.cookie;
  s.selectedArchive=c.selectedArchive;s.selectedEvaluated=c.selectedEvaluated;
  s.budget.add(c.bytes);
  const auto shape=[&](const auto& table,size_t before,const auto& tail) {
    if(tail.empty()) return; // This command has no ownership in this domain.
    if(table.size()!=before+(forward?0:tail.size())) throw std::invalid_argument("Material resource owned tail is not at its original vacancy");
    if(!forward && !std::equal(tail.begin(),tail.end(),table.begin()+before,[](const auto& a,const auto& b){return sameResourceRecord(a,b);}))
      throw std::invalid_argument("Material resource owned tail records changed");
  };
  shape(t.aImages,c.aImages,c.aImageTail);shape(t.eImages,c.eImages,c.eImageTail);
  shape(t.aSamplers,c.aSamplers,c.aSamplerTail);shape(t.eSamplers,c.eSamplers,c.eSamplerTail);
  shape(t.aTextures,c.aTextures,c.aTextureTail);shape(t.eTextures,c.eTextures,c.eTextureTail);
  if((!c.runtimeTail.empty() && t.textures.textureCount()!=c.runtime+(forward?0:c.runtimeTail.size())) ||
      (!c.requestTail.empty() && t.requests.size()!=c.requests+(forward?0:c.requestTail.size())))
    throw std::invalid_argument("Material resource runtime/cache tail is not at its original vacancy");
  if(!forward) {
    for(size_t i=0;i<c.runtimeTail.size();++i) {
      const auto* row=t.textures.getTexture(uint32_t(c.runtime+i));
      if(!row || !sameTexture(*row,c.runtimeTail[i]))
        throw std::invalid_argument("Material resource owned runtime tail changed");
    }
    for(size_t i=0;i<c.requestTail.size();++i) {
      const auto& row=t.requests[c.requests+i];const auto& owned=c.requestTail[i];
      if(row.key!=owned.key || row.encoded!=owned.encoded || row.archive!=owned.archive || row.evaluated!=owned.evaluated ||
          row.sourceIdentity!=owned.sourceIdentity || row.inputSourceIdentity!=owned.inputSourceIdentity || row.active!=owned.active)
        throw std::invalid_argument("Material resource owned cache request changed");
    }
    if((!c.mapTail.empty() && (t.mapping.size()!=c.aTextures+c.mapTail.size() || !std::equal(c.mapTail.begin(),c.mapTail.end(),t.mapping.begin()+c.aTextures))) ||
        (!c.identityTail.empty() && (t.identities.size()!=c.aTextures+c.identityTail.size() || !std::equal(c.identityTail.begin(),c.identityTail.end(),t.identities.begin()+c.aTextures))))
      throw std::invalid_argument("Material resource owned source identity changed");
  }
  for(const auto& change:c.roles) {
    const auto& a=change.color?t.aRoles.color:t.aRoles.data;const auto& e=change.color?t.eRoles.color:t.eRoles.data;
    if(change.archive<a.size() && a[change.archive]!=(forward?change.beforeA:change.after))
      throw std::invalid_argument("Material resource command lost its authored role ownership");
    if(change.evaluated<e.size() && e[change.evaluated]!=(forward?change.beforeE:change.after))
      throw std::invalid_argument("Material resource command lost its evaluated role ownership");
  }
  for(const auto& change:c.mappings)
    if(change.archive>=t.mapping.size() || t.mapping[change.archive]!=(forward?change.before:change.after))
      throw std::invalid_argument("Material resource command lost its source mapping ownership");
  for(const auto& change:c.identities)
    if(change.archive>=t.identities.size() || t.identities[change.archive]!=(forward?change.before:change.after))
      throw std::invalid_argument("Material resource command lost its source identity ownership");
  if(!forward && (!c.aImageTail.empty() || !c.eImageTail.empty() || !c.aSamplerTail.empty() || !c.eSamplerTail.empty() ||
      !c.aTextureTail.empty() || !c.eTextureTail.empty() || !c.runtimeTail.empty())) {
    const auto aTextures=c.aTextureTail.empty()?t.aTextures.size():c.aTextures;
    const auto eTextures=c.eTextureTail.empty()?t.eTextures.size():c.eTextures;
    const auto aImages=c.aImageTail.empty()?t.aImages.size():c.aImages;
    const auto eImages=c.eImageTail.empty()?t.eImages.size():c.eImages;
    const auto aSamplers=c.aSamplerTail.empty()?t.aSamplers.size():c.aSamplers;
    const auto eSamplers=c.eSamplerTail.empty()?t.eSamplers.size():c.eSamplers;
    const auto runtimeCount=c.runtimeTail.empty()?t.textures.textureCount():c.runtime;
    container::material::GltfMaterialConversionLimits validation;validation.maxSourceTextures=limits.maxSourceCount;
    validation.validation.maxOutputBytes=limits.maxPreparedCpuBytes;validation.validation.maxMetadataValues=limits.maxMetadataValues;
    validation.validation.maxMetadataDepth=limits.maxMetadataDepth;
    const auto inspectMaterials=[&](const auto& materials,size_t target,size_t current,auto& witness) {
      for(size_t i=0;i<materials.size();++i) {
        const auto bytes=container::geometry::gltf::GltfMaterialRecordStorageBytes(materials[i],validation.validation);s.budget.add(bytes);
        const auto traversal=bytes/sizeof(tinygltf::Value)+1;
        if(traversal>s.budget.values) throw std::length_error("Material resource reference records exceed their aggregate work budget");
        s.budget.values-=traversal;
        // Validate every known raw/fallback index in the smaller domain, not
        // only the effective23-slot result (a shadowed fallback still counts).
        (void)container::material::ReadGltfMaterialSourceTextures(materials[i],i==size_t(c.material)?current:target,validation);
      }
      witness=materials;
    };
    inspectMaterials(exportSource_.materials,aTextures,t.aTextures.size(),s.referenceAuthored);
    inspectMaterials(gltfModel_.materials,eTextures,t.eTextures.size(),s.referenceEvaluated);
    s.budget.add(materialManager_.materialCount(),sizeof(container::geometry::gltf::ImportedMaterialTextureBindings));
    s.referenceRuntime.reserve(materialManager_.materialCount());
    const auto changingRuntime=resolveLoadedMaterialIndex(c.material);
    for(uint32_t runtime=0;runtime<materialManager_.materialCount();++runtime) {
      const auto* material=materialManager_.getMaterial(runtime);
      if(!material) throw std::invalid_argument("Runtime material reference audit found an unavailable row");
      const auto refs=container::geometry::gltf::snapshotMaterialTextureBindings(*material);
      if(runtime!=changingRuntime) for(auto ref:refs.runtimeIndices)
        if(ref!=UINT32_MAX && ref>=runtimeCount) throw std::invalid_argument("Another runtime material references the owned texture resource tail");
      s.referenceRuntime.push_back(refs);
    }
    const auto imageSamplerRefs=[&](const auto& textures,size_t retained,size_t images,size_t samplers) {
      for(size_t i=0;i<retained;++i) {
        if(!s.budget.values) throw std::length_error("Material texture reference audit exceeds its work limit");--s.budget.values;
        const auto& texture=textures[i];
        if(texture.source < -1 || (texture.source>=0 && size_t(texture.source)>=images) ||
            texture.sampler < -1 || (texture.sampler>=0 && size_t(texture.sampler)>=samplers))
          throw std::invalid_argument("A retained texture references an owned image/sampler tail");
        for(const auto& [extension,body]:texture.extensions) {
          if(extension!="KHR_texture_basisu" && extension!="EXT_texture_webp" && extension!="MSFT_texture_dds") continue;
          if(!body.IsObject()) throw std::invalid_argument("Texture image-source extension must be an object");
          const tinygltf::Value* index=nullptr;
          for(const auto& [name,value]:body.Get<tinygltf::Value::Object>()) if(name=="source") {index=&value;break;}
          if(!index || !index->IsInt() || index->GetNumberAsInt()<0 || size_t(index->GetNumberAsInt())>=images)
            throw std::invalid_argument("A retained alternate texture source references an owned image tail");
        }
      }
    };
    imageSamplerRefs(t.aTextures,aTextures,aImages,aSamplers);imageSamplerRefs(t.eTextures,eTextures,eImages,eSamplers);
    const auto modelAudit=[&](const tinygltf::Model& model,size_t textures,size_t images,size_t samplers,auto& witnesses,const auto& originalUnknowns) {
      size_t count{};
      walkResourceExtensions(model,[&](ResourceDomain domain,const auto& extensions,std::string_view raw,uint64_t location) {
        if(!s.budget.values) throw std::length_error("Material resource normative audit exceeds its work limit");--s.budget.values;
        s.budget.add(sizeof(PreparedMaterialResources::State::ExtensionWitness)+raw.size()+64);
        const uint64_t kind=location>>56,index=(location>>28)&0x0fffffffull;
        const bool removedRow=(kind==13 && index>=images) || (kind==14 && index>=samplers) || (kind==15 && index>=textures);
        for(const auto& [name,body]:extensions) {
          s.budget.add(name.size()+64);s.budget.value(body);
          // References inside a row removed by this command disappear with
          // that row. Its exact current metadata is still pinned below.
          if(removedRow) continue;
          const auto inspectUnknown=[&](std::span<const ResourceUnknownStep> steps,const tinygltf::Value& unknown) {
            const auto path=resourceUnknownPath(steps,s.budget);
            bool retained=false;
            for(const auto& expected:originalUnknowns) {
              if(!s.budget.values) throw std::length_error("Material resource unknown-extension audit exceeds its work limit");--s.budget.values;
              if(expected.location==location && expected.domain==uint8_t(domain) && expected.name==name && expected.path==path && sameResourceValue(expected.body,unknown)) {retained=true;break;}
            }
            if(!retained) throw std::invalid_argument("A changed unknown normative extension may retain owned texture resources");
          };
          if(!resourceExtensionAllowed(name,domain)) inspectUnknown({},body);
          else auditResourceNested(body,s.budget,inspectUnknown);
          if(domain==ResourceDomain::Target && name=="KHR_animation_pointer") auditResourcePointer(model,body,textures,images,samplers);
        }
        ++count;return true;
      });
      witnesses.reserve(count);
      walkResourceExtensions(model,[&](ResourceDomain domain,const auto& extensions,std::string_view raw,uint64_t) {
        witnesses.push_back({uint8_t(domain),extensions,std::string(raw)});return true;
      });
    };
    modelAudit(exportSource_,aTextures,aImages,aSamplers,s.referenceExtensionsA,c.unknownA);
    modelAudit(gltfModel_,eTextures,eImages,eSamplers,s.referenceExtensionsE,c.unknownE);
  }
  const auto changeTable=[&](auto& table,size_t before,const auto& tail) {
    if(tail.empty()) return;
    if(forward) {for(const auto& row:tail) s.budget.record(row);table.insert(table.end(),tail.begin(),tail.end());}
    else table.resize(before);
  };
  changeTable(t.aImages,c.aImages,c.aImageTail);changeTable(t.eImages,c.eImages,c.eImageTail);
  changeTable(t.aSamplers,c.aSamplers,c.aSamplerTail);changeTable(t.eSamplers,c.eSamplers,c.eSamplerTail);
  changeTable(t.aTextures,c.aTextures,c.aTextureTail);changeTable(t.eTextures,c.eTextures,c.eTextureTail);
  if(forward) {
    s.budget.add(c.requestTail.size(),sizeof(MaterialTextureSourceRequest));
    for(const auto& request:c.requestTail) s.budget.add(request.key.size());
    t.requests.insert(t.requests.end(),c.requestTail.begin(),c.requestTail.end());
    t.mapping.insert(t.mapping.end(),c.mapTail.begin(),c.mapTail.end());t.identities.insert(t.identities.end(),c.identityTail.begin(),c.identityTail.end());
  } else {
    if(!c.requestTail.empty()) t.requests.resize(c.requests);
    if(!c.mapTail.empty()) t.mapping.resize(c.aTextures);
    if(!c.identityTail.empty()) t.identities.resize(c.aTextures);
  }
  if(!c.runtimeTail.empty()) {
    container::material::RuntimeTextureRetirementLimits runtimeLimits;runtimeLimits.maxOwnedBytes=s.budget.remaining;
    s.runtimeAppend.emplace(container::material::PrepareRuntimeTextureAppendReplay(textureManager_,c.runtimeAppend,forward,runtimeLimits));
    s.budget.add(s.runtimeAppend->ownedBytes());t.textures=s.runtimeAppend->textures();
  }
  t.aRoles.color.resize(t.aTextures.size(),UINT32_MAX);t.aRoles.data.resize(t.aTextures.size(),UINT32_MAX);
  t.eRoles.color.resize(t.eTextures.size(),UINT32_MAX);t.eRoles.data.resize(t.eTextures.size(),UINT32_MAX);
  for(const auto& change:c.roles) {
    auto& a=change.color?t.aRoles.color:t.aRoles.data;auto& e=change.color?t.eRoles.color:t.eRoles.data;
    if(change.archive<a.size()) a[change.archive]=forward?change.after:change.beforeA;
    if(change.evaluated<e.size()) e[change.evaluated]=forward?change.after:change.beforeE;
  }
  for(const auto& change:c.mappings) t.mapping[change.archive]=forward?change.after:change.before;
  for(const auto& change:c.identities) t.identities[change.archive]=forward?change.after:change.before;
  t.exportIndices.resize(t.textures.textureCount(),-1);
  if(forward) for(const auto& change:c.roles) if(change.after!=UINT32_MAX) t.exportIndices[change.after]=int32_t(change.archive);
  const auto declarations=[&](auto& current,const auto& owned,const auto& textures) {
    for(const auto& name:owned) {
      if(forward) {
        if(std::find(current.begin(),current.end(),name)==current.end()) {s.budget.add(name.size()+sizeof(std::string));current.push_back(name);}
      } else {
        bool used=false;
        for(const auto& texture:textures) {
          if(!s.budget.values) throw std::length_error("Material resource declaration replay exceeds its work limit");--s.budget.values;
          for(const auto& [extension,value]:texture.extensions) {if(extension==name) {used=true;break;}}
          if(used) break;
        }
        if(!used) std::erase(current,name);
      }
    }
  };
  declarations(t.aUsed,c.aUsed,t.aTextures);declarations(t.eUsed,c.eUsed,t.eTextures);
  declarations(t.aRequired,c.aRequired,t.aTextures);declarations(t.eRequired,c.eRequired,t.eTextures);
  result.finishPreparation();return result;
}

void SceneManager::PreparedMaterialResources::prepareSource(std::optional<uint32_t> source,const std::string& request,
    const std::string& name,std::span<const std::byte> bytes,MaterialTextureRoles roles,
    std::optional<uint32_t> requestedSampler,const container::material::MaterialTextureSamplerDescription* authoredSampler) {
  if(!*this || state_->ready) throw std::invalid_argument("Material resource source was already prepared");
  auto& s=*state_;auto& t=s.next;const auto& owner=*s.owner;
  if(uint8_t(roles)<1 || uint8_t(roles)>3) throw std::invalid_argument("Material texture roles are invalid");
  if(requestedSampler) (void)container::material::DecodeMaterialSamplerKey({*requestedSampler});
  if(authoredSampler) {
    const auto& policy=*authoredSampler;
    requestedSampler=container::material::NormalizeMaterialSamplerKey(policy.minFilter,policy.magFilter,policy.wrapS,policy.wrapT).value;
  }
  if(!source && (bytes.empty() || bytes.size()>s.limits.maxEncodedBytes))
    throw std::length_error("Material texture encoded bytes exceed their capacity");
  s.budget.add(bytes.size(),2);s.budget.add(name.size()+request.size()+128,4);
  uint32_t archive=UINT32_MAX,sampler=requestedSampler.value_or(0),decodeSource=UINT32_MAX;
  const auto samplerSuffix=[&] {
    auto suffix="#sampler="+std::to_string(sampler);
    // Source aliases distinguish omitted defaults from explicit defaults,
    // while runtime sampler slots still use the canonical key alone.
    if(authoredSampler && authoredSampler->minFilter==-1) suffix+="#min=omitted";
    if(authoredSampler && authoredSampler->magFilter==-1) suffix+="#mag=omitted";
    return suffix;
  };
  std::string key;
  std::shared_ptr<const uint8_t> inputIdentity;
  bool newSource=false;
  if(source) {
    if(*source>=t.aTextures.size() || *source>=t.identities.size() || !t.identities[*source])
      throw std::invalid_argument("Chosen material texture source identity is unavailable");
    if(!t.aPhases.textures.empty() && (*source>=t.aPhases.textures.size() || t.aPhases.textures[*source]!=1))
      throw std::invalid_argument("Chosen material texture source is retired");
    archive=*source;decodeSource=*source;
    container::material::MaterialTextureSamplerDescription originalPolicy;
    uint32_t originalSampler{};const auto oldSampler=t.aTextures[archive].sampler;
    if(oldSampler>=0) {
      if(size_t(oldSampler)>=t.aSamplers.size()) throw std::invalid_argument("Chosen material texture sampler is unavailable");
      const auto& old=t.aSamplers[size_t(oldSampler)];
      originalPolicy={old.minFilter,old.magFilter,old.wrapS,old.wrapT};
      originalSampler=container::material::NormalizeMaterialSamplerKey(old.minFilter,old.magFilter,old.wrapS,old.wrapT).value;
    }
    sampler=requestedSampler.value_or(originalSampler);
    if(authoredSampler ? *authoredSampler!=originalPolicy : sampler!=originalSampler) {
      inputIdentity=t.identities[archive];
      key="source:"+std::to_string(archive)+samplerSuffix();
      archive=UINT32_MAX;
      for(const auto& cached:t.requests) {
        if(!cached.active) continue;
        if(cached.key!=key || cached.inputSourceIdentity!=inputIdentity) continue;
        if(cached.archive>=t.identities.size() || cached.sourceIdentity!=t.identities[cached.archive])
          throw std::invalid_argument("Sampler alias source identity is stale");
        if(!t.aPhases.textures.empty() && (cached.archive>=t.aPhases.textures.size() || t.aPhases.textures[cached.archive]!=1)) continue;
        archive=cached.archive;break;
      }
      newSource=archive==UINT32_MAX;
    }
  } else {
    key=request+samplerSuffix()+"#size="+std::to_string(bytes.size())+"#bytes="+std::to_string(hashEncoded(bytes));
    uint64_t comparisons{};
    for(const auto& cached:t.requests) {
      if(!cached.active) continue;
      if(cached.key!=key || !cached.encoded || cached.encoded->size()!=bytes.size()) continue;
      if(bytes.size()>s.limits.maxPreparedCpuBytes-comparisons) throw std::length_error("Material resource cache comparison exceeds its work budget");
      comparisons+=bytes.size();
      if(!std::equal(bytes.begin(),bytes.end(),cached.encoded->begin())) continue;
      if(cached.archive>=t.identities.size() || cached.sourceIdentity!=t.identities[cached.archive])
        throw std::invalid_argument("Material texture source cache identity is stale");
      if(!t.aPhases.textures.empty() && (cached.archive>=t.aPhases.textures.size() || t.aPhases.textures[cached.archive]!=1)) continue;
      archive=cached.archive;decodeSource=archive;break;
    }
    newSource=archive==UINT32_MAX;
  }
  uint32_t preliminaryNeeded{};
  for(bool color:{true,false}) if(resourceRole(roles,color)) {
    const auto& registered=color?t.aRoles.color:t.aRoles.data;
    if(newSource || archive>=registered.size() || registered[archive]==UINT32_MAX) ++preliminaryNeeded;
    else if(!t.textures.isTextureActive(registered[archive]))
      throw std::invalid_argument("Chosen material texture role references a retired runtime resource");
  }
  if(preliminaryNeeded>s.textureCapacity-t.textures.textureCount())
    throw std::length_error("Material texture exceeds sampled-image descriptor capacity");
  {
    s.budget.add(t.textures.textureCount()+preliminaryNeeded+1,sizeof(container::material::MaterialSamplerKey)*2);
    const std::array<container::material::MaterialSamplerKey,2> policies{{{sampler},{sampler}}};
    (void)container::material::PlanTextureSamplerSlots(t.textures,s.samplerCapacity,std::span(policies.data(),preliminaryNeeded));
  }
  const auto countGuard=[&](size_t count,uint64_t extra=1) {
    if(count>s.limits.maxSourceCount || extra>s.limits.maxSourceCount-count)
      throw std::length_error("Material texture resource tables exceed their identity capacity");
  };
  const auto appendDeclaration=[&](auto& names,const std::string& extension) {
    for(const auto& current:names) {if(!s.budget.values) throw std::length_error("Material resource declaration work exceeds its limit");--s.budget.values;if(current==extension)return;}
    s.budget.add(extension.size()+sizeof(std::string),4);names.push_back(extension);
  };
  const auto declareTexture=[&](const tinygltf::Texture& texture) {
    for(const auto& [extension,value]:texture.extensions) {
      (void)value;appendDeclaration(t.aUsed,extension);appendDeclaration(t.eUsed,extension);
      if(extension=="KHR_texture_basisu" || extension=="EXT_texture_webp" || extension=="MSFT_texture_dds") {
        // A texture without a core fallback requires its image-source codec.
        if(texture.source<0) {appendDeclaration(t.aRequired,extension);appendDeclaration(t.eRequired,extension);}
      }
    }
  };
  const auto copyImage=[&](uint32_t image)->uint32_t {
    if(image>=t.aImages.size()) throw std::invalid_argument("Material source image is unavailable");
    if(!t.aPhases.images.empty() && (image>=t.aPhases.images.size() || t.aPhases.images[image]!=1))
      throw std::invalid_argument("Material source image is retired");
    countGuard(t.eImages.size());s.budget.record(t.aImages[image],2);
    auto copied=t.aImages[image];
    if(copied.image.empty()) throw std::invalid_argument("Material source image has no retained pixel/encoded payload");
    // TinyGLTF retains selected image bytes. Keeping that standalone payload
    // avoids copying a buffer which may also contain geometry/accessor bytes.
    copied.bufferView=-1;copied.uri.clear();
    const auto index=uint32_t(t.eImages.size());t.eImages.push_back(std::move(copied));return index;
  };
  const auto evaluatedCopy=[&](const tinygltf::Texture& authored)->tinygltf::Texture {
    s.budget.record(authored,2);auto copied=authored;
    if(copied.source>=0) copied.source=int(copyImage(uint32_t(copied.source)));
    for(const auto* extension:{"KHR_texture_basisu","EXT_texture_webp","MSFT_texture_dds"}) {
      const auto found=copied.extensions.find(extension);if(found==copied.extensions.end()) continue;
      if(!found->second.IsObject()) throw std::invalid_argument("Material image-source extension must be an object");
      auto& fields=found->second.Get<tinygltf::Value::Object>();const auto field=fields.find("source");
      if(field==fields.end() || !field->second.IsInt() || field->second.GetNumberAsInt()<0)
        throw std::invalid_argument("Material image-source extension has an invalid image index");
      field->second=tinygltf::Value(int(copyImage(uint32_t(field->second.GetNumberAsInt()))));
      copied.extensions_json_string.clear();
    }
    return copied;
  };
  if(newSource) {
    countGuard(t.aTextures.size());countGuard(t.eTextures.size());countGuard(t.aSamplers.size());countGuard(t.eSamplers.size());countGuard(t.requests.size());
    const auto policy=container::material::DecodeMaterialSamplerKey({sampler});
    tinygltf::Sampler samplerRecord;
    samplerRecord.minFilter=authoredSampler?authoredSampler->minFilter:policy.minFilter;
    samplerRecord.magFilter=authoredSampler?authoredSampler->magFilter:policy.magFilter;
    samplerRecord.wrapS=policy.wrapS;samplerRecord.wrapT=policy.wrapT;s.budget.record(samplerRecord,4);
    tinygltf::Texture aTexture,eTexture;
    if(source) {
      s.budget.record(t.aTextures[*source],2);aTexture=t.aTextures[*source];
      const auto mapping=t.mapping[*source];
      if(mapping>=0) {
        if(size_t(mapping)>=t.eTextures.size()) throw std::invalid_argument("Material evaluated source identity is unavailable");
        s.budget.record(t.eTextures[size_t(mapping)],2);eTexture=t.eTextures[size_t(mapping)];
      } else eTexture=evaluatedCopy(aTexture);
      aTexture.extensions_json_string.clear();eTexture.extensions_json_string.clear();
    } else {
      countGuard(t.aImages.size());countGuard(t.eImages.size());
      auto options=s.allocation->textureDecodeOptions(false);
      if(!options.maxDecodedBytes || options.maxDecodedBytes==UINT64_MAX || !options.maxEncodedBytes ||
          options.maxEncodedBytes==UINT64_MAX || options.maxMipLevels>32 || options.allowedFormats.size()>1024)
        throw std::invalid_argument("Material resource retention requires finite device capacities");
      const auto raw=std::span(reinterpret_cast<const unsigned char*>(bytes.data()),bytes.size());
      const bool recognized=!container::geometry::gltf::EncodedTextureMimeType(raw).empty();
      const auto capacity=recognized?uint64_t(bytes.size()):std::min(options.maxDecodedBytes,s.budget.remaining/4);
      if(!capacity) throw std::length_error("Material texture has no remaining retained-image capacity");
      s.budget.add(capacity,4);options.maxDecodedBytes=capacity;options.maxEncodedBytes=std::min(options.maxEncodedBytes,s.limits.maxEncodedBytes);
      auto retained=container::geometry::gltf::RetainTextureImageForExport(bytes,options);
      if(retained.image.size()>capacity) throw std::length_error("Retained material image exceeds its capacity");
      s.budget.remaining+=(capacity-retained.image.size())*4;
      const auto aImage=uint32_t(t.aImages.size()),eImage=uint32_t(t.eImages.size());
      const auto mime=retained.mimeType;retained.name=name;retained.uri.clear();retained.bufferView=-1;
      t.aImages.push_back(retained);t.eImages.push_back(std::move(retained));
      const char* extension=mime=="image/webp"?"EXT_texture_webp":mime=="image/vnd-ms.dds"?"MSFT_texture_dds":mime=="image/ktx2"?"KHR_texture_basisu":nullptr;
      if(extension) {
        aTexture.extensions[extension]=tinygltf::Value(tinygltf::Value::Object{{"source",tinygltf::Value(int(aImage))}});
        eTexture.extensions[extension]=tinygltf::Value(tinygltf::Value::Object{{"source",tinygltf::Value(int(eImage))}});
      } else {aTexture.source=int(aImage);eTexture.source=int(eImage);}
      aTexture.name=name;eTexture.name=name;
    }
    aTexture.sampler=int(t.aSamplers.size());eTexture.sampler=int(t.eSamplers.size());
    t.aSamplers.push_back(samplerRecord);t.eSamplers.push_back(std::move(samplerRecord));
    archive=uint32_t(t.aTextures.size());const auto evaluated=uint32_t(t.eTextures.size());
    declareTexture(aTexture);t.aTextures.push_back(std::move(aTexture));t.eTextures.push_back(std::move(eTexture));
    t.aRoles.color.resize(t.aTextures.size(),UINT32_MAX);t.aRoles.data.resize(t.aTextures.size(),UINT32_MAX);
    t.eRoles.color.resize(t.eTextures.size(),UINT32_MAX);t.eRoles.data.resize(t.eTextures.size(),UINT32_MAX);
    t.mapping.resize(t.aTextures.size(),-1);t.mapping[archive]=int32_t(evaluated);
    t.identities.resize(t.aTextures.size());t.identities[archive]=std::make_shared<const uint8_t>(0);
    std::shared_ptr<const std::vector<std::byte>> encoded;
    if(!source) encoded=std::make_shared<const std::vector<std::byte>>(bytes.begin(),bytes.end());
    s.budget.add(sizeof(MaterialTextureSourceRequest)+key.size()+128,2);
    t.requests.push_back({key,std::move(encoded),archive,evaluated,t.identities[archive],std::move(inputIdentity)});
  }
  if(archive>=t.aTextures.size() || archive>=t.mapping.size() || archive>=t.identities.size() || !t.identities[archive])
    throw std::invalid_argument("Material source texture has incomplete private provenance");
  if(t.mapping[archive]<0) {
    countGuard(t.eTextures.size());auto texture=evaluatedCopy(t.aTextures[archive]);
    if(texture.sampler>=0) {
      if(size_t(texture.sampler)>=t.aSamplers.size()) throw std::invalid_argument("Material source sampler is unavailable");
      countGuard(t.eSamplers.size());s.budget.record(t.aSamplers[size_t(texture.sampler)],2);
      auto copied=t.aSamplers[size_t(texture.sampler)];texture.sampler=int(t.eSamplers.size());t.eSamplers.push_back(std::move(copied));
    }
    declareTexture(texture);t.mapping[archive]=int32_t(t.eTextures.size());t.eTextures.push_back(std::move(texture));
    t.eRoles.color.resize(t.eTextures.size(),UINT32_MAX);t.eRoles.data.resize(t.eTextures.size(),UINT32_MAX);
    t.eRoles.color[size_t(t.mapping[archive])]=t.aRoles.color[archive];
    t.eRoles.data[size_t(t.mapping[archive])]=t.aRoles.data[archive];
  }
  const auto evaluated=uint32_t(t.mapping[archive]);if(evaluated>=t.eTextures.size()) throw std::invalid_argument("Material evaluated texture identity is invalid");
  uint32_t needed{};
  for(bool color:{true,false}) if(resourceRole(roles,color)) {
    const auto& a=color?t.aRoles.color:t.aRoles.data;const auto& e=color?t.eRoles.color:t.eRoles.data;
    if(a[archive]==UINT32_MAX) ++needed;
    else if(!t.textures.isTextureActive(a[archive]) || e[evaluated]!=a[archive])
      throw std::invalid_argument("Material authored/evaluated texture roles disagree");
  }
  if(needed>s.textureCapacity-t.textures.textureCount()) throw std::length_error("Material texture exceeds sampled-image descriptor capacity");
  s.budget.add(t.textures.textureCount()+needed+1,sizeof(container::material::MaterialSamplerKey)*2);
  const std::array<container::material::MaterialSamplerKey,2> policies{{{sampler},{sampler}}};
  (void)container::material::PlanTextureSamplerSlots(t.textures,s.samplerCapacity,std::span(policies.data(),needed));
  std::optional<container::gpu::AllocationManager::TextureAllocationBatch> batch;
  for(bool color:{true,false}) {
    if(!resourceRole(roles,color)) continue;auto& a=color?t.aRoles.color:t.aRoles.data;auto& e=color?t.eRoles.color:t.eRoles.data;
    if(a[archive]!=UINT32_MAX) continue;
    container::material::TextureResource resource;
    const auto& originalRoles=color?owner.archiveTextureResources_.color:owner.archiveTextureResources_.data;
    const bool alias=source && *source<originalRoles.size() && originalRoles[*source]!=UINT32_MAX;
    if(alias) {
      const auto* original=owner.textureManager_.getTexture(originalRoles[*source]);
      if(!original || original->imageView==VK_NULL_HANDLE) throw std::invalid_argument("Sampler edit has no retained image role to share");
      resource=*original;
    } else {
      auto options=s.allocation->textureDecodeOptions(color);
      if(!options.maxDecodedBytes || options.maxDecodedBytes==UINT64_MAX || options.maxMipLevels>32 || options.allowedFormats.size()>1024)
        throw std::invalid_argument("Material resource decoding requires finite device capacities");
      options.maxEncodedBytes=std::min(options.maxEncodedBytes,s.limits.maxEncodedBytes);
      options.maxDecodedBytes=std::min({options.maxDecodedBytes,s.budget.remaining/2,s.limits.maxNewTextureBytes-s.newTextureBytes});
      if(!options.maxDecodedBytes) throw std::length_error("Material texture has no remaining decoded-image capacity");
      container::material::TextureImageData decoded;
      if(!bytes.empty()) {
        options.strictKhrBasisu=t.aTextures[archive].extensions.contains("KHR_texture_basisu");
        decoded=container::material::DecodeTextureImage(bytes,options);
      } else {
        if(decodeSource==UINT32_MAX) decodeSource=archive;
        const auto selected=container::geometry::gltf::SelectTextureImageSource(owner.exportSource_,int(decodeSource));
        decoded=container::geometry::gltf::DecodeSelectedTextureImage(owner.exportSource_,selected,
            color?container::geometry::gltf::TextureImageRole::Color:container::geometry::gltf::TextureImageRole::Data,options);
      }
      uint64_t decodedBytes{};for(const auto& mip:decoded.mips) {
        if(mip.bytes.size()>options.maxDecodedBytes-decodedBytes) throw std::length_error("Material texture decoded bytes exceed their limit");
        decodedBytes+=mip.bytes.size();
      }
      s.budget.add(decodedBytes,2);const auto gpuBytes=uploadedImageBytes(decoded);
      if(gpuBytes>s.limits.maxNewTextureBytes-s.newTextureBytes) throw std::length_error("Material texture allocation exceeds its logical GPU byte budget");
      s.newTextureBytes+=gpuBytes;
      if(!batch) batch.emplace(s.allocation->beginTextureAllocationBatch());
      resource=s.allocation->createTextureFromImageData(
          name.empty()?"material-source-"+std::to_string(archive):name,decoded,owner.materialQueueFamilies());
    }
    s.budget.add(sizeof(resource)+key.size()+resource.contentKey.size()+192,3);
    resource.name="material-source:"+std::to_string(archive)+(color?"#color":"#data")+"#sampler="+std::to_string(sampler);
    resource.samplerIndex=sampler;
    if(t.textures.findTextureIndex(resource.name)) throw std::invalid_argument("Material resource name exists without its explicit source role proof");
    const auto runtime=t.textures.registerTexture(resource);a[archive]=runtime;e[evaluated]=runtime;
    t.exportIndices.resize(t.textures.textureCount(),-1);t.exportIndices[runtime]=int32_t(archive);
  }
  if(batch) {
    s.budget.add(sizeof(container::gpu::AllocationManager::TextureAllocationLease)+128);
    s.stagedBatch=batch->commit();
  }
  s.selectedArchive=archive;s.selectedEvaluated=evaluated;
  if(source) s.inputArchives[0]=*source;
  if(archive<s.before.aTextures.size() && s.inputArchives[0]!=archive)
    s.inputArchives[s.inputArchives[0]==UINT32_MAX?0:1]=archive;
}

void SceneManager::PreparedMaterialResources::finishPreparation() {
  if(!*this || state_->ready) throw std::invalid_argument("Material resources are already prepared");
  auto& s=*state_;auto& t=s.next;const auto& b=s.before;
  const auto resizePhases=[&](auto& phases,size_t images,size_t textures,size_t samplers) {
    if(!phases.images.empty() && images>phases.images.size()) s.budget.add(images-phases.images.size());
    if(!phases.textures.empty() && textures>phases.textures.size()) s.budget.add(textures-phases.textures.size());
    if(!phases.samplers.empty() && samplers>phases.samplers.size()) s.budget.add(samplers-phases.samplers.size());
    phases.resize(images,textures,samplers);
  };
  resizePhases(t.aPhases,t.aImages.size(),t.aTextures.size(),t.aSamplers.size());
  resizePhases(t.ePhases,t.eImages.size(),t.eTextures.size(),t.eSamplers.size());
  if(!s.replay && t.textures.textureCount()>b.textures.textureCount()) {
    container::material::RuntimeTextureRetirementLimits runtimeLimits;runtimeLimits.maxOwnedBytes=s.budget.remaining;
    s.runtimeAppend.emplace(container::material::PrepareRuntimeTextureAppend(s.owner->textureManager_,t.textures,runtimeLimits));
    s.budget.add(s.runtimeAppend->ownedBytes());
  }
  const auto ownershipBytes=container::material::TextureImageOwnershipStorageBytes(t.textures.textureCount());
  s.budget.add(ownershipBytes);
  const std::array inherited{b.imageOwnership,s.replay?s.command.state_->imageOwnership:ResourceImageOwnership{}};
  // Recompute complete surviving coverage on both replay directions. A logical
  // sampler tail can share an image with the retained prefix, so ownership must
  // never be updated by subtracting the command's selected pins.
  t.imageOwnership=container::material::PrepareTextureImageOwnership(t.textures,*s.allocation,inherited,ownershipBytes);
  s.budget.add(s.owner->runtimeMaterialProvenance_.size(),sizeof(RuntimeMaterialProvenance));
  s.candidateProvenance=s.owner->runtimeMaterialProvenance_;
  const auto changedRuntime=s.owner->sourceMaterialToRuntime_[size_t(s.sourceMaterial)];
  for(uint32_t runtime=0;runtime<s.candidateProvenance.size();++runtime) {
    auto& row=s.candidateProvenance[runtime];if(!row.known) continue;
    const auto* material=s.owner->materialManager_.getMaterial(runtime);
    if(!material) throw std::invalid_argument("Material resource provenance exceeds its runtime table");
    try {row.slots=forwardProvenance(row.slots,*material);}
    catch(const std::invalid_argument&) {
      // The paired material exchange replaces this exact selected row next.
      // Every other retained runtime material must remain valid immediately.
      if(runtime!=changedRuntime || !s.replay || s.forward) throw;
      row={};
    }
  }
  if(!s.replay) {
    const uint64_t commandStart=s.budget.remaining;
    const auto tailCharge=[&](const auto& before,const auto& after) {
      if(after.size()<before.size()) throw std::invalid_argument("Initial material resource operation cannot remove a resource prefix");
      for(size_t i=before.size();i<after.size();++i) s.budget.record(after[i]);
    };
    tailCharge(b.aImages,t.aImages);tailCharge(b.eImages,t.eImages);tailCharge(b.aSamplers,t.aSamplers);tailCharge(b.eSamplers,t.eSamplers);
    tailCharge(b.aTextures,t.aTextures);tailCharge(b.eTextures,t.eTextures);
    for(size_t i=b.textures.textureCount();i<t.textures.textureCount();++i) {
      const auto* resource=t.textures.getTexture(uint32_t(i));
      if(!resource) throw std::invalid_argument("A newly appended material resource tail must contain active rows");
      s.budget.add(sizeof(*resource)+resource->name.size()+resource->contentKey.size()+128);
    }
    for(size_t i=b.requests.size();i<t.requests.size();++i) {
      const auto& request=t.requests[i];s.budget.add(sizeof(request)+request.key.size()+64);
      if(request.encoded) s.budget.add(request.encoded->size());
    }
    s.budget.add(t.mapping.size()-b.mapping.size(),sizeof(int32_t)+sizeof(std::shared_ptr<const uint8_t>));
    s.budget.add(sizeof(MaterialResourceCommand::State)+128);
    s.command.state_=std::make_shared<MaterialResourceCommand::State>();auto& c=*s.command.state_;
    c.owner=s.owner;c.ownerIdentity=s.ownerIdentity;c.resourceOwner=s.resourceOwner;c.asset=s.asset;c.material=s.sourceMaterial;
    if(s.runtimeAppend) c.runtimeAppend=s.runtimeAppend->command();
    c.fallback=s.fallback;
    c.cookie=std::make_shared<const uint8_t>(0);c.selectedArchive=s.selectedArchive;c.selectedEvaluated=s.selectedEvaluated;
    c.aImages=b.aImages.size();c.aSamplers=b.aSamplers.size();c.aTextures=b.aTextures.size();
    c.eImages=b.eImages.size();c.eSamplers=b.eSamplers.size();c.eTextures=b.eTextures.size();
    c.runtime=b.textures.textureCount();c.requests=b.requests.size();
    // Unknown normative metadata predating this command cannot acquire new
    // suffix references while its exact bytes stay unchanged. Retain only
    // those extension bodies, never a historical model or resource prefix.
    const auto retainUnknowns=[&](const tinygltf::Model& model,auto& target) {
      walkResourceExtensions(model,[&](ResourceDomain domain,const auto& fields,std::string_view,uint64_t location) {
        if(!s.budget.values) throw std::length_error("Material resource unknown-extension snapshot exceeds its work limit");--s.budget.values;
        for(const auto& [name,body]:fields) {
          const auto retain=[&](std::span<const ResourceUnknownStep> steps,const tinygltf::Value& unknown) {
            auto path=resourceUnknownPath(steps,s.budget);
            s.budget.add(sizeof(MaterialResourceCommand::State::UnknownExtension)+name.size()+64);s.budget.value(unknown);
            target.push_back({location,uint8_t(domain),name,std::move(path),unknown});
          };
          if(!resourceExtensionAllowed(name,domain)) retain({},body);
          else auditResourceNested(body,s.budget,retain);
        }
        return true;
      });
    };
    retainUnknowns(s.owner->exportSource_,c.unknownA);retainUnknowns(s.owner->gltfModel_,c.unknownE);
    const auto copyTail=[](const auto& before,const auto& after,auto& target) {target.assign(after.begin()+before.size(),after.end());};
    copyTail(b.aImages,t.aImages,c.aImageTail);copyTail(b.eImages,t.eImages,c.eImageTail);
    copyTail(b.aSamplers,t.aSamplers,c.aSamplerTail);copyTail(b.eSamplers,t.eSamplers,c.eSamplerTail);
    copyTail(b.aTextures,t.aTextures,c.aTextureTail);copyTail(b.eTextures,t.eTextures,c.eTextureTail);
    copyTail(b.requests,t.requests,c.requestTail);copyTail(b.mapping,t.mapping,c.mapTail);copyTail(b.identities,t.identities,c.identityTail);
    for(size_t i=b.textures.textureCount();i<t.textures.textureCount();++i) {
      const auto* resource=t.textures.getTexture(uint32_t(i));
      if(!resource) throw std::invalid_argument("Material history cannot capture an inactive appended resource tail");
      c.runtimeTail.push_back(*resource);
    }
    const auto selectedRecord=[&](const auto& rows,uint32_t index,auto& witnesses) {
      if(index>=rows.size()) throw std::invalid_argument("Material resource command input index is unavailable");
      if(std::ranges::any_of(witnesses,[&](const auto& item){return item.first==index;})) return;
      s.budget.add(sizeof(uint32_t)+64);s.budget.record(rows[index]);witnesses.emplace_back(index,rows[index]);
    };
    const auto selectedTexture=[&](const auto& table,uint32_t index,const auto& images,const auto& samplers,
        auto& textureWitnesses,auto& imageWitnesses,auto& samplerWitnesses) {
      selectedRecord(table,index,textureWitnesses);const auto& texture=table[index];
      if(texture.source>=0) selectedRecord(images,uint32_t(texture.source),imageWitnesses);
      if(texture.sampler>=0) selectedRecord(samplers,uint32_t(texture.sampler),samplerWitnesses);
      for(const auto& [extension,body]:texture.extensions) {
        if(extension!="KHR_texture_basisu" && extension!="EXT_texture_webp" && extension!="MSFT_texture_dds") continue;
        if(!body.IsObject()) throw std::invalid_argument("Material input image-source extension must be an object");
        const tinygltf::Value* field=nullptr;
        for(const auto& [name,value]:body.Get<tinygltf::Value::Object>()) if(name=="source") {field=&value;break;}
        if(!field || !field->IsInt() || field->GetNumberAsInt()<0) throw std::invalid_argument("Material input image-source index is invalid");
        selectedRecord(images,uint32_t(field->GetNumberAsInt()),imageWitnesses);
      }
    };
    for(const auto input:s.inputArchives) if(input!=UINT32_MAX) {
      selectedTexture(b.aTextures,input,b.aImages,b.aSamplers,c.prefixATextures,c.prefixAImages,c.prefixASamplers);
      if(input>=b.identities.size() || !b.identities[input]) throw std::invalid_argument("Material input source has no private identity");
      s.budget.add(sizeof(decltype(c.prefixIdentities)::value_type));c.prefixIdentities.emplace_back(input,b.identities[input]);
      s.budget.add(sizeof(decltype(c.prefixMappings)::value_type));c.prefixMappings.emplace_back(input,b.mapping[input]);
      if(b.mapping[input]>=0) selectedTexture(b.eTextures,uint32_t(b.mapping[input]),b.eImages,b.eSamplers,c.prefixETextures,c.prefixEImages,c.prefixESamplers);
    }
    for(size_t archive=0;archive<t.aTextures.size();++archive) {
      if(!s.budget.values) throw std::length_error("Material resource role audit exceeds its work limit");--s.budget.values;
      const auto mapped=t.mapping[archive];
      if(archive<b.mapping.size() && b.mapping[archive]!=mapped) {
        s.budget.add(sizeof(MaterialResourceCommand::State::Mapping));c.mappings.push_back({uint32_t(archive),b.mapping[archive],mapped});
      }
      if(archive<b.identities.size() && b.identities[archive]!=t.identities[archive]) {
        s.budget.add(sizeof(MaterialResourceCommand::State::Identity));c.identities.push_back({uint32_t(archive),b.identities[archive],t.identities[archive]});
      }
      if(mapped<0) continue;
      for(bool color:{true,false}) {
        const auto& current=color?t.aRoles.color:t.aRoles.data;const auto& live=color?t.eRoles.color:t.eRoles.data;
        const auto& oldA=color?b.aRoles.color:b.aRoles.data;const auto& oldE=color?b.eRoles.color:b.eRoles.data;
        const auto beforeA=archive<oldA.size()?oldA[archive]:UINT32_MAX;
        const auto beforeE=size_t(mapped)<oldE.size()?oldE[size_t(mapped)]:UINT32_MAX;
        if(current[archive]!=live[size_t(mapped)]) throw std::invalid_argument("Prepared material source roles disagree");
        if(current[archive]!=beforeA || current[archive]!=beforeE) {
          s.budget.add(sizeof(MaterialResourceCommand::State::Role));c.roles.push_back({uint32_t(archive),uint32_t(mapped),beforeA,beforeE,current[archive],color});
        }
      }
    }
    const auto added=[&](const auto& before,const auto& after,auto& target) {
      for(const auto& name:after) {
        bool found=false;for(const auto& old:before) {
          if(!s.budget.values) throw std::length_error("Material resource declaration audit exceeds its work limit");--s.budget.values;
          if(name==old) {found=true;break;}
        }
        if(!found) {s.budget.add(name.size()+sizeof(std::string));target.push_back(name);}
      }
    };
    added(b.aUsed,t.aUsed,c.aUsed);added(b.eUsed,t.eUsed,c.eUsed);added(b.aRequired,t.aRequired,c.aRequired);added(b.eRequired,t.eRequired,c.eRequired);
    const auto slots=t.textures.textureCount();
    s.budget.add(slots,sizeof(uint8_t)+sizeof(uint32_t));
    std::vector<uint8_t> selectedMask(slots,0);
    std::vector<uint32_t> selected;selected.reserve(slots);
    const auto select=[&](uint32_t runtime) {
      if(runtime==UINT32_MAX) return;
      if(runtime>=slots) throw std::invalid_argument("Material resource history image selection exceeds its runtime table");
      // Inactive source-role rows are not actual GPU owners. Binding such a
      // role still rejects; no fallback capability is authorized for history.
      if(!t.textures.isTextureActive(runtime) || selectedMask[runtime]) return;
      selectedMask[runtime]=1;selected.push_back(runtime);
    };
    for(size_t runtime=c.runtime;runtime<slots;++runtime) select(uint32_t(runtime));
    for(const auto& role:c.roles) {select(role.beforeA);select(role.beforeE);select(role.after);}
    const auto selectArchive=[&](const auto& roles,uint32_t archive) {
      if(archive<roles.color.size()) select(roles.color[archive]);
      if(archive<roles.data.size()) select(roles.data[archive]);
    };
    selectArchive(t.aRoles,s.selectedArchive);selectArchive(b.aRoles,s.selectedArchive);
    for(const auto input:s.inputArchives) if(input!=UINT32_MAX) {
      selectArchive(t.aRoles,input);selectArchive(b.aRoles,input);
    }
    const auto selectedBytes=container::material::TextureImageOwnershipStorageBytes(selected.size());
    s.budget.add(selectedBytes);
    c.imageOwnership=container::material::SelectTextureImageOwnership(t.imageOwnership,selected,*s.allocation,selectedBytes);
    c.bytes=commandStart-s.budget.remaining+c.runtimeAppend.ownedBytes();
  }
  TextureResourceLifecycleLimits logicalLimits;logicalLimits.maxOwnedBytes=s.budget.remaining;
  // Owned-tail replay explicitly invalidates obsolete prepared generations,
  // matching material-tail replay. Arbitrary unused-resource Delete will keep
  // its independent reference proof and default BlockPreparations policy.
  if(s.replay) logicalLimits.retirementPolicy=container::util::ResourceRetirementPolicy::InvalidatePreparations;
  s.logicalChange=s.replay?
      PrepareTextureResourceLifecycleReplay(b.logicalGeneration,s.nextLifecycleView(),s.command.state_->logicalCommand,s.forward,logicalLimits):
      PrepareTextureResourceLifecycle(b.logicalGeneration,s.nextLifecycleView(),logicalLimits);
  t.logicalGeneration=s.logicalChange.next();s.budget.add(s.logicalChange.ownedBytes());
  s.logicalBytesAccounted=s.logicalChange.ownedBytes();
  if(!s.replay) {
    auto& c=*s.command.state_;c.logicalCommand=s.logicalChange.command();
    const auto chargeBefore=s.budget.remaining;
    std::vector<TextureResourceSelection> selected;
    const auto selectRuntime=[&](uint32_t row) {
      if(row!=UINT32_MAX && t.textures.isTextureActive(row))
        selected.push_back({container::util::ResourceDomainKind::Runtime,container::util::ResourceKind::RuntimeTexture,row});
    };
    const auto selectArchive=[&](uint32_t row) {
      if(row>=t.aTextures.size()) return;
      selected.push_back({container::util::ResourceDomainKind::Archive,container::util::ResourceKind::Texture,row});
      if(row<t.mapping.size() && t.mapping[row]>=0)
        selected.push_back({container::util::ResourceDomainKind::Evaluated,container::util::ResourceKind::Texture,uint32_t(t.mapping[row])});
      if(row<t.aRoles.color.size()) selectRuntime(t.aRoles.color[row]);
      if(row<t.aRoles.data.size()) selectRuntime(t.aRoles.data[row]);
    };
    const auto selectionCapacity=t.textures.textureCount()+3*c.roles.size()+24;
    s.budget.add(selectionCapacity,sizeof(TextureResourceSelection));
    selected.reserve(selectionCapacity);
    for(size_t row=c.runtime;row<t.textures.textureCount();++row) selectRuntime(uint32_t(row));
    for(const auto& role:c.roles) {selectRuntime(role.beforeA);selectRuntime(role.beforeE);selectRuntime(role.after);}
    selectArchive(s.selectedArchive);for(const auto input:s.inputArchives) if(input!=UINT32_MAX) selectArchive(input);
    auto selectedOwner=s.logicalChange.select(selected,container::util::ResourcePinClass::History,s.budget.remaining,
        !c.unknownA.empty() || !c.unknownE.empty());
    s.budget.add(selectedOwner->ownedBytes());
    const std::array owners{c.logicalCommand.references(),selectedOwner};
    c.logicalReferences=CombineTextureResourceReferences(owners,s.budget.remaining);
    s.budget.add(c.logicalReferences->ownedBytes());
    const auto runtimeMaterial=s.owner->sourceMaterialToRuntime_[size_t(s.sourceMaterial)];
    if(!s.owner->materialLifecycle_ || runtimeMaterial>=s.owner->materialLifecycleHandles_.size())
      throw std::invalid_argument("Material resource history has no selected material identity");
    c.logicalMaterial=s.owner->materialLifecycleHandles_[runtimeMaterial];
    auto materialPin=s.owner->materialLifecycle_->pin(std::span(&c.logicalMaterial,1),container::util::ResourcePinClass::History,
        "Selected material resource history");
    c.logicalReferences=AddTextureResourceReferencePin(c.logicalReferences,std::move(materialPin),s.budget.remaining);
    s.budget.add(c.logicalReferences->ownedBytes());
    c.bytes+=c.logicalCommand.ownedBytes()+(chargeBefore-s.budget.remaining);
  }
  s.futureLogicalPreparation=s.logicalChange.selectAll(container::util::ResourcePinClass::Preparation,s.budget.remaining);
  s.budget.add(s.futureLogicalPreparation->ownedBytes());
  auto descriptors=s.owner->prepareTextureDescriptorState(t.textures,s.budget.remaining,s.limits.maxMetadataBufferBytes);
  s.budget.add(descriptors->ownedBytes);
  s.sampling=std::move(descriptors->sampling);s.slots=std::move(descriptors->slots);
  s.images=std::move(descriptors->images);s.metadata=std::move(descriptors->metadata);
  s.samplers=std::move(descriptors->samplers);s.createdSamplers.swap(descriptors->createdSamplers);
  s.descriptorSamplers=std::move(descriptors->descriptorSamplers);
  std::swap(s.metadataBuffer,descriptors->metadataBuffer);
  // Complete next-table coverage and selected command pins are already held.
  // Never let aggregate destruction or a final image pin release occur inside
  // the later allocation-free resource/material publication.
  s.stagedBatch.reset();
  s.ready=true;
}

void SceneManager::PreparedMaterialResources::writeDescriptorSetContents(VkDescriptorSet set,size_t imageIndex,
    const container::gpu::AllocatedBuffer& camera,const container::gpu::AllocatedBuffer& objects,
    const container::gpu::AllocatedBuffer& materials,size_t materialCount) const {
  if(!*this || !state_->ready || !state_->owner->canExchangeMaterialResources(*this))
    throw std::invalid_argument("Material resources are not current and ready for descriptors");
  const auto& s=*state_;
  s.owner->writeTextureDescriptorSetContents(set,imageIndex,camera,objects,materials,materialCount,
      s.metadataBuffer,s.metadata.size(),s.images,s.descriptorSamplers);
}

SceneManager::PreparedTextureDescriptorState::~PreparedTextureDescriptorState() {
  if(metadataBuffer.buffer!=VK_NULL_HANDLE && allocation) allocation->destroyBuffer(metadataBuffer);
  if(device) for(const auto sampler:createdSamplers) if(sampler!=VK_NULL_HANDLE)
    destroyOwnedSampler(device->device(),sampler,nullptr);
}

std::shared_ptr<SceneManager::PreparedTextureDescriptorState> SceneManager::prepareTextureDescriptorState(
    const container::material::TextureManager& textures,uint64_t maxCpuBytes,uint64_t maxMetadataBufferBytes) const {
  using namespace container::material;
  if(!maxCpuBytes || maxCpuBytes==UINT64_MAX || !maxMetadataBufferBytes || maxMetadataBufferBytes==UINT64_MAX ||
      !allocationManager_ || !deviceWrapper_ || !textureSamplingFallbackValid() ||
      materialSamplers_.size()!=kMaterialSamplerKeyCount || !materialSamplerDescriptorCapacity_)
    throw std::invalid_argument("Texture descriptor preparation requires finite budgets and owned fallback resources");
  Budget budget;budget.remaining=maxCpuBytes;budget.add(sizeof(PreparedTextureDescriptorState)+128);
  budget.add(std::max<size_t>(1,textures.textureCount()),sizeof(VkDescriptorImageInfo)+sizeof(container::gpu::GpuTextureMetadata));
  budget.add(kMaterialSamplerKeyCount,sizeof(MaterialSamplerKey)+2*sizeof(VkSampler));
  budget.add(materialSamplerDescriptorCapacity_,sizeof(VkSampler));
  const auto planBytes=TextureSamplingPlanStorageBytes(textures.textureCount());budget.add(planBytes);
  auto result=std::make_shared<PreparedTextureDescriptorState>();result->allocation=allocationManager_;result->device=deviceWrapper_;result->fallback=textureSamplingFallback_;
  result->sampling=MakeTextureSamplingPlan(textures,result->fallback->resource,*allocationManager_,textureDescriptorCapacity_,materialSamplerDescriptorCapacity_,planBytes);
  result->slots=result->sampling.samplerSlots;result->images=result->sampling.images;result->metadata=result->sampling.metadata;
  result->samplers=materialSamplers_;result->createdSamplers.reserve(kMaterialSamplerKeyCount);
  for(const auto key:result->slots.keys) {
    if(key.value>=result->samplers.size()) throw std::invalid_argument("Texture sampler key exceeds the cache capacity");
    if(result->samplers[key.value]!=VK_NULL_HANDLE) continue;
    const auto info=MakeMaterialSamplerCreateInfo(key);VkSampler sampler{};
    if(createOwnedSampler(deviceWrapper_->device(),&info,nullptr,&sampler)!=VK_SUCCESS) throw std::runtime_error("Failed to create detached texture sampler");
    result->createdSamplers.push_back(sampler);result->samplers[key.value]=sampler;
  }
  if(result->samplers[0]==VK_NULL_HANDLE) throw std::invalid_argument("Texture default sampler is unavailable");
  result->descriptorSamplers.reserve(materialSamplerDescriptorCapacity_);
  for(uint32_t slot=0;slot<materialSamplerDescriptorCapacity_;++slot)
    result->descriptorSamplers.push_back(slot<result->slots.keys.size()?result->samplers[result->slots.keys[slot].value]:result->samplers[0]);
  const auto bytes=result->metadata.size()*sizeof(container::gpu::GpuTextureMetadata);
  VkPhysicalDeviceProperties physical{};vkGetPhysicalDeviceProperties(deviceWrapper_->physicalDevice(),&physical);
  if(bytes>maxMetadataBufferBytes || bytes>physical.limits.maxStorageBufferRange)
    throw std::length_error("Texture metadata exceeds its GPU buffer capacity");
  result->metadataBuffer=createMaterialStorageBuffer(bytes);
  auto* mapped=result->metadataBuffer.allocation_info.pMappedData;bool mappedHere=false;
  const auto allocator=allocationManager_->memoryManager()->allocator();
  if(!mapped) {
    if(vmaMapMemory(allocator,result->metadataBuffer.allocation,&mapped)!=VK_SUCCESS) throw std::runtime_error("Failed to map detached texture metadata");
    mappedHere=true;
  }
  std::memcpy(mapped,result->metadata.data(),bytes);const auto flushed=vmaFlushAllocation(allocator,result->metadataBuffer.allocation,0,bytes);
  if(mappedHere) vmaUnmapMemory(allocator,result->metadataBuffer.allocation);
  if(flushed!=VK_SUCCESS) throw std::runtime_error("Failed to flush detached texture metadata");
  result->ownedBytes=maxCpuBytes-budget.remaining;return result;
}

bool SceneManager::textureDescriptorStateMatches(const PreparedTextureDescriptorState& state,
    const container::material::TextureManager& textures) const noexcept {
  if(!allocationManager_ || state.allocation!=allocationManager_ || state.device!=deviceWrapper_ || !state.fallback ||
      state.fallback!=textureSamplingFallback_ || !textureSamplingFallbackValid() || state.metadataBuffer.buffer==VK_NULL_HANDLE ||
      state.metadataBuffer.allocation==VK_NULL_HANDLE || state.slots.keys!=state.sampling.samplerSlots.keys ||
      state.slots.slots!=state.sampling.samplerSlots.slots || state.samplers.size()!=materialSamplers_.size() ||
      state.metadata.size()!=state.sampling.metadata.size() || state.images.size()!=state.sampling.images.size() ||
      state.descriptorSamplers.size()!=materialSamplerDescriptorCapacity_ ||
      !container::material::TextureSamplingPlanMatches(state.sampling,textures,state.fallback->resource,*allocationManager_)) return false;
  for(size_t row=0;row<state.metadata.size();++row)
    if(std::memcmp(&state.metadata[row],&state.sampling.metadata[row],sizeof(container::gpu::GpuTextureMetadata))) return false;
  for(size_t row=0;row<state.images.size();++row) {
    const auto& a=state.images[row];const auto& b=state.sampling.images[row];
    if(a.imageView!=b.imageView || a.imageLayout!=b.imageLayout || a.sampler!=b.sampler) return false;
  }
  if(state.samplers.empty() || state.samplers[0]==VK_NULL_HANDLE) return false;
  for(size_t key=0;key<materialSamplers_.size();++key)
    if(materialSamplers_[key]!=VK_NULL_HANDLE && state.samplers[key]!=materialSamplers_[key]) return false;
  for(size_t slot=0;slot<state.descriptorSamplers.size();++slot) {
    const auto key=slot<state.slots.keys.size()?state.slots.keys[slot].value:0;
    if(key>=state.samplers.size() || state.samplers[key]==VK_NULL_HANDLE || state.descriptorSamplers[slot]!=state.samplers[key]) return false;
  }
  return true;
}

void SceneManager::exchangeTextureDescriptorState(PreparedTextureDescriptorState& state) noexcept {
  using std::swap;textureMetadata_.swap(state.metadata);swap(textureMetadataBuffer_,state.metadataBuffer);
  swap(textureSamplingPlan_,state.sampling);swap(materialSamplerSlots_,state.slots);textureMetadataBufferCapacity_=textureMetadata_.size();
  for(size_t key=0;key<materialSamplers_.size();++key) if(materialSamplers_[key]==VK_NULL_HANDLE) materialSamplers_[key]=state.samplers[key];
  state.createdSamplers.clear();baseColorSampler_=materialSamplers_[0];
}

void SceneManager::writeTextureDescriptorSetContents(VkDescriptorSet set,size_t imageIndex,
    const container::gpu::AllocatedBuffer& camera,const container::gpu::AllocatedBuffer& objects,
    const container::gpu::AllocatedBuffer& materials,size_t materialCount,const PreparedTextureDescriptorState& state) const {
  writeTextureDescriptorSetContents(set,imageIndex,camera,objects,materials,materialCount,state.metadataBuffer,state.metadata.size(),state.images,state.descriptorSamplers);
}

void SceneManager::writeTextureDescriptorSetContents(VkDescriptorSet set,size_t imageIndex,
    const container::gpu::AllocatedBuffer& camera,const container::gpu::AllocatedBuffer& objects,
    const container::gpu::AllocatedBuffer& materials,size_t materialCount,const container::gpu::AllocatedBuffer& metadata,
    size_t metadataCount,std::span<const VkDescriptorImageInfo> images,std::span<const VkSampler> descriptorSamplers) const {
  if(set==VK_NULL_HANDLE || camera.buffer==VK_NULL_HANDLE || objects.buffer==VK_NULL_HANDLE || materials.buffer==VK_NULL_HANDLE || !materialCount)
    throw std::invalid_argument("Material texture descriptor inputs are unavailable");
  const std::array buffers{
      VkDescriptorBufferInfo{camera.buffer,0,sizeof(container::gpu::CameraData)},
      VkDescriptorBufferInfo{objects.buffer,0,VK_WHOLE_SIZE},
      VkDescriptorBufferInfo{materials.buffer,0,sizeof(container::gpu::GpuMaterial)*materialCount},
      VkDescriptorBufferInfo{metadata.buffer,0,sizeof(container::gpu::GpuTextureMetadata)*metadataCount},
      VkDescriptorBufferInfo{sceneClipBuffer(imageIndex).buffer,0,sizeof(container::gpu::SceneClipState)}};
  std::array<VkWriteDescriptorSet,7> writes{};size_t count{};
  constexpr std::array<uint32_t,5> bindings{0,1,3,4,6};
  for(size_t i=0;i<buffers.size();++i) {
    auto& write=writes[count++];write.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;write.dstSet=set;write.dstBinding=bindings[i];
    write.descriptorType=i==0 || i==4?VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.descriptorCount=1;write.pBufferInfo=&buffers[i];
  }
  if(!images.empty()) {
    auto& write=writes[count++];write.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;write.dstSet=set;write.dstBinding=5;
    write.descriptorType=VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;write.descriptorCount=uint32_t(images.size());write.pImageInfo=images.data();
  }
  std::vector<VkDescriptorImageInfo> samplers(descriptorSamplers.size());
  for(size_t i=0;i<samplers.size();++i) samplers[i].sampler=descriptorSamplers[i];
  auto& write=writes[count++];write.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;write.dstSet=set;write.dstBinding=2;
  write.descriptorType=VK_DESCRIPTOR_TYPE_SAMPLER;write.descriptorCount=uint32_t(samplers.size());write.pImageInfo=samplers.data();
  vkUpdateDescriptorSets(deviceWrapper_->device(),uint32_t(count),writes.data(),0,nullptr);
}

bool SceneManager::canPrepareMaterialResources(const PreparedMaterialResources& token) const noexcept {
  if(!token || !token.state_->ready || token.state_->stagedBatch) return false;const auto& s=*token.state_;const auto& b=s.before;
  if(s.runtimeAppend && !container::material::CanExchangeRuntimeTextureAppend(*s.runtimeAppend,textureManager_)) return false;
  if(b.logicalGeneration!=textureResourceLifecycle_ || !textureResourceLifecycle_ || !materialResourceDependenciesValid() ||
      !textureResourceLifecycle_->matches(textureResourceLifecycleView()) ||
      !s.beforeLogicalReaders || !s.beforeLogicalReaders->valid() ||
      !s.futureLogicalPreparation || !s.futureLogicalPreparation->valid() ||
      !CanPrepareTextureResourceLifecycle(s.logicalChange,textureResourceLifecycle_,s.nextLifecycleView())) return false;
  if(s.owner!=this || s.ownerIdentity!=sceneRootOwnerIdentity_ || s.resourceOwner!=materialResourceOwnerIdentity_ ||
      s.epoch!=generationEpoch_ || s.materialRevision!=materialRevision_ || s.authoredRevision!=authoredMaterialRevision_ ||
      s.geometryRevision!=geometryRevision_ || s.asset!=materialAssetIdentity() || s.resourceRevision!=materialResourceRevision_ ||
      s.expectedVacancy!=materialResourceVacancy_ || materialResourceRevision_==UINT64_MAX ||
      s.materialCount!=materialCount() || s.sourceCount!=gltfModel_.materials.size() || s.sourceCount!=exportSource_.materials.size() ||
      s.sourceMaterial<0 || size_t(s.sourceMaterial)>=sourceMaterialToRuntime_.size() ||
      s.pool!=descriptorPool_ || s.layout!=descriptorSetLayout_ || s.materialBuffer!=materialBuffer_.buffer ||
      s.metadataBefore!=textureMetadataBuffer_.buffer || s.metadataAllocationBefore!=textureMetadataBuffer_.allocation ||
      s.beforeMetadataCount!=textureMetadata_.size() || s.clipFrames!=sceneClipFrameIdentity() ||
      s.samplingRevisionBefore!=textureSamplingPlan_.textureRevision || s.samplingRowsBefore!=textureSamplingPlan_.rows.size() ||
      s.textureCapacity!=textureDescriptorCapacity_ || s.samplerCapacity!=materialSamplerDescriptorCapacity_ ||
      !s.fallback || !s.fallback->lease || s.fallback!=textureSamplingFallback_ ||
      !textureSamplingStateValidForResources() ||
      !container::material::TextureSamplingPlanMatches(s.sampling,s.next.textures,s.fallback->resource,*allocationManager_) ||
      s.expectedSamplers!=materialSamplers_ || s.next.textures.textureCount()>textureDescriptorCapacity_ ||
      !sameResourceRecords(b.aImages,exportSource_.images) || !sameResourceRecords(b.eImages,gltfModel_.images) ||
      !sameResourceRecords(b.aSamplers,exportSource_.samplers) || !sameResourceRecords(b.eSamplers,gltfModel_.samplers) ||
      !sameResourceRecords(b.aTextures,exportSource_.textures) || !sameResourceRecords(b.eTextures,gltfModel_.textures) ||
      b.aUsed!=exportSource_.extensionsUsed || b.eUsed!=gltfModel_.extensionsUsed ||
      b.aRequired!=exportSource_.extensionsRequired || b.eRequired!=gltfModel_.extensionsRequired ||
      b.aRoles.color!=archiveTextureResources_.color || b.aRoles.data!=archiveTextureResources_.data ||
      b.aPhases!=archiveResourcePhases_ || b.ePhases!=gltfResourcePhases_ ||
      b.eRoles.color!=gltfTextureResources_.color || b.eRoles.data!=gltfTextureResources_.data || b.mapping!=archiveTextureToEvaluated_ ||
      b.identities!=archiveTextureIdentities_ || b.exportIndices!=exportTextureIndices_ || b.imageOwnership!=textureImageOwnership_ ||
      !container::material::TextureImageOwnershipMatches(b.imageOwnership,textureManager_,*allocationManager_) ||
      !container::material::TextureImageOwnershipMatches(s.next.imageOwnership,s.next.textures,*allocationManager_) ||
      b.requests.size()!=materialTextureSourceRequests_.size() || !sameTextures(b.textures,textureManager_)) return false;
  for(size_t i=0;i<b.requests.size();++i) {
    const auto& a=b.requests[i];const auto& live=materialTextureSourceRequests_[i];
    if(a.key!=live.key || a.encoded!=live.encoded || a.archive!=live.archive || a.evaluated!=live.evaluated ||
        a.sourceIdentity!=live.sourceIdentity || a.inputSourceIdentity!=live.inputSourceIdentity || a.active!=live.active) return false;
  }
  if(!s.referenceAuthored.empty()) {
    if(s.referenceAuthored.size()!=exportSource_.materials.size() || s.referenceEvaluated.size()!=gltfModel_.materials.size() ||
        s.referenceRuntime.size()!=materialManager_.materialCount()) return false;
    for(size_t i=0;i<s.referenceAuthored.size();++i)
      if(!container::geometry::gltf::GltfMaterialRecordsEqual(s.referenceAuthored[i],exportSource_.materials[i]) ||
          !container::geometry::gltf::GltfMaterialRecordsEqual(s.referenceEvaluated[i],gltfModel_.materials[i])) return false;
    for(uint32_t i=0;i<s.referenceRuntime.size();++i) {
      const auto* value=materialManager_.getMaterial(i);
      if(!value || container::geometry::gltf::snapshotMaterialTextureBindings(*value).runtimeIndices!=s.referenceRuntime[i].runtimeIndices) return false;
    }
    const auto extensionsMatch=[](const tinygltf::Model& model,const auto& witnesses) noexcept {
      size_t next{};
      const bool matched=walkResourceExtensions(model,[&](ResourceDomain domain,const auto& fields,std::string_view raw,uint64_t) noexcept {
        if(next>=witnesses.size()) return false;const auto& expected=witnesses[next++];
        return expected.domain==uint8_t(domain) && expected.raw==raw && sameResourceExtensions(expected.fields,fields);
      });
      return matched && next==witnesses.size();
    };
    if(!extensionsMatch(exportSource_,s.referenceExtensionsA) || !extensionsMatch(gltfModel_,s.referenceExtensionsE)) return false;
  }
  if(!s.command) return false;const auto& c=*s.command.state_;
  if(c.owner!=this || c.ownerIdentity!=sceneRootOwnerIdentity_ || c.resourceOwner!=materialResourceOwnerIdentity_ || c.asset!=materialAssetIdentity() ||
      c.fallback!=s.fallback || !c.fallback || !c.fallback->lease ||
      !container::material::TextureImageOwnershipValid(c.imageOwnership,*allocationManager_) ||
      !c.logicalReferences || !c.logicalReferences->valid() ||
      size_t(s.sourceMaterial)>=sourceMaterialToRuntime_.size() ||
      sourceMaterialToRuntime_[size_t(s.sourceMaterial)]>=materialLifecycleHandles_.size() ||
      materialLifecycleHandles_[sourceMaterialToRuntime_[size_t(s.sourceMaterial)]]!=c.logicalMaterial ||
      c.material!=s.sourceMaterial || c.phase!=(s.replay?(s.forward?MaterialResourceCommand::State::Phase::Detached:MaterialResourceCommand::State::Phase::Present):MaterialResourceCommand::State::Phase::Unpublished) ||
      (s.replay && s.forward && materialResourceVacancy_!=c.cookie)) return false;
  return true;
}

bool SceneManager::canExchangeMaterialResources(const PreparedMaterialResources& token) const noexcept {
  return canPrepareMaterialResources(token) && token.state_->logicalChange.sealed() &&
      (token.state_->pairedMaterialPublication || materialResourceDependencyMapWillBeActive(
          token.state_->standaloneMaterialDependencies,token.state_->logicalChange)) &&
      CanExchangeTextureResourceLifecycle(token.state_->logicalChange,textureResourceLifecycle_,token.state_->nextLifecycleView());
}

void SceneManager::exchangeMaterialResources(PreparedMaterialResources& token) noexcept {
  if(!canExchangeMaterialResources(token)) return;auto& s=*token.state_;auto& t=s.next;
  const bool logicalPublished=ExchangeTextureResourceLifecycle(s.logicalChange,textureResourceLifecycle_,s.nextLifecycleView());
  assert(logicalPublished);(void)logicalPublished;
  if(!s.pairedMaterialPublication) materialResourceDependencies_.swap(s.standaloneMaterialDependencies);
  using std::swap;
  exportSource_.images.swap(t.aImages);gltfModel_.images.swap(t.eImages);exportSource_.samplers.swap(t.aSamplers);gltfModel_.samplers.swap(t.eSamplers);
  exportSource_.textures.swap(t.aTextures);gltfModel_.textures.swap(t.eTextures);
  swap(archiveResourcePhases_,t.aPhases);swap(gltfResourcePhases_,t.ePhases);
  exportSource_.extensionsUsed.swap(t.aUsed);gltfModel_.extensionsUsed.swap(t.eUsed);
  exportSource_.extensionsRequired.swap(t.aRequired);gltfModel_.extensionsRequired.swap(t.eRequired);
  archiveTextureResources_.color.swap(t.aRoles.color);archiveTextureResources_.data.swap(t.aRoles.data);
  gltfTextureResources_.color.swap(t.eRoles.color);gltfTextureResources_.data.swap(t.eRoles.data);
  archiveTextureToEvaluated_.swap(t.mapping);archiveTextureIdentities_.swap(t.identities);materialTextureSourceRequests_.swap(t.requests);
  exportTextureIndices_.swap(t.exportIndices);
  if(s.runtimeAppend) {
    const bool runtimePublished=container::material::ExchangeRuntimeTextureAppend(*s.runtimeAppend,textureManager_);
    assert(runtimePublished);(void)runtimePublished;
  } else swap(textureManager_,t.textures);
  textureImageOwnership_.swap(t.imageOwnership);
  runtimeMaterialProvenance_.swap(s.candidateProvenance);
  textureMetadata_.swap(s.metadata);swap(textureMetadataBuffer_,s.metadataBuffer);textureMetadataBufferCapacity_=textureMetadata_.size();
  swap(textureSamplingPlan_,s.sampling);
  // Published canonical samplers form an append-only, fixed-key scene cache.
  // Undo changes dense slots/metadata; redo can reuse these handles directly.
  for(size_t i=0;i<materialSamplers_.size();++i) if(materialSamplers_[i]==VK_NULL_HANDLE) materialSamplers_[i]=s.samplers[i];
  s.createdSamplers.clear();swap(materialSamplerSlots_,s.slots);baseColorSampler_=materialSamplers_[0];
  auto& c=*s.command.state_;
  if(s.replay && !s.forward) {c.childVacancy=s.expectedVacancy;c.phase=MaterialResourceCommand::State::Phase::Detached;}
  else c.phase=MaterialResourceCommand::State::Phase::Present;
  materialResourceVacancy_=s.futureVacancy;++materialResourceRevision_;s.consumed=true;
}
} // namespace container::scene
