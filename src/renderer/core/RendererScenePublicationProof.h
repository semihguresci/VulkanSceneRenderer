#pragma once

#include <Container/renderer/bim/BimManager.h>
#include <Container/scene/SceneProvider.h>
#include <Container/utility/SceneManager.h>

#include <algorithm>
#include <array>
#include <limits>
#include <optional>
#include <stdexcept>
#include <vector>

namespace container::renderer::detail {

// Shared exact external-reader proof for a prepared scene publication.
// Snapshot and identity comparisons run without allocation at commit.
struct ScenePublicationExternalProof {
  const BimManager* bim{};
  uint64_t geometry{},objects{},metadata{};
  VkBuffer buffer{};
  bool available{};
  std::vector<uint32_t> objectMaterials,metadataMaterials;
  std::array<const container::scene::IRenderSceneProvider*,16> providers{};
  std::vector<container::scene::SceneProviderSnapshot> providerStates;
  const container::scene::SceneManager* resourceManager{};
  uint64_t resourceAsset{},resourceMaterials{},resourceAuthored{};
  std::vector<container::util::ResourceHandle> materialIdentities;
  size_t providerCount{};
  // Borrowed admission only: no snapshot(), record copies or vector/string
  // allocations. Include conservative temporary/capacity slack and the same
  // immutable batch residency counted by ownedBytes(). The caller charges
  // this before constructing the proof, then verifies actual retained bytes.
  [[nodiscard]] static uint64_t preparationBytes(const BimManager* source,
      const container::scene::SceneProviderRegistry& registry,size_t materialCount,
      const container::scene::SceneManager* resourceOwner=nullptr) {
    uint64_t bytes=sizeof(ScenePublicationExternalProof)+4096;
    const auto add=[&](uint64_t count,uint64_t stride) {
      if(stride && count>(std::numeric_limits<uint64_t>::max()-bytes)/stride)
        throw std::length_error("External reader proof preparation size overflows");
      bytes+=count*stride;
    };
    if(materialCount>1'000'000 || (resourceOwner && resourceOwner->materialCount()!=materialCount))
      throw std::length_error("Resource consumer material identity proof exceeds its limit");
    if(resourceOwner) add(materialCount,2*sizeof(container::util::ResourceHandle));
    if(source) {
      const auto objects=source->objectData().size();const auto metadata=source->elementMetadata().size();
      if(objects>1'000'000 || metadata>1'000'000)
        throw std::length_error("Material external-reference proof exceeds its limit");
      add(objects,2*sizeof(uint32_t));add(metadata,2*sizeof(uint32_t));
    }
    const auto current=registry.providers();
    if(current.size()>16) throw std::length_error("Material provider proof exceeds its limit");
    add(current.size(),2*sizeof(container::scene::SceneProviderSnapshot)+256);
    for(const auto* provider:current) {
      if(!provider) throw std::invalid_argument("A retained material provider is unavailable");
      const auto references=provider->materialReferenceView();
      if(!references || !references->complete)
        throw std::invalid_argument("A provider's material reference capability is incomplete");
      if(references->id.size()>65536 || references->displayName.size()>65536 || references->triangleBatches.size()>1'000'000)
        throw std::length_error("Material provider metadata exceeds its proof limit");
      const bool primary=dynamic_cast<const container::scene::MeshSceneProvider*>(provider) && references->id=="primary-mesh-scene";
      const bool auxiliary=dynamic_cast<const container::scene::BimSceneProvider*>(provider) && references->id=="auxiliary-bim-scene";
      const bool independent=(dynamic_cast<const container::scene::GaussianSplatSceneProvider*>(provider) && references->id=="gaussian-splat-scene") ||
          (dynamic_cast<const container::scene::RadianceFieldSceneProvider*>(provider) && references->id=="radiance-field-scene");
      if(!primary && !auxiliary && !independent)
        throw std::invalid_argument("A provider's material reference domain cannot be proved");
      // The temporary snapshot and its retained copy can coexist. Small-string
      // capacities and allocation rounding remain covered for empty labels.
      add(references->id.size()+32,4);add(references->displayName.size()+32,4);
      add(references->triangleBatches.size(),sizeof(container::scene::SceneProviderTriangleBatch));
    }
    return bytes;
  }
  ScenePublicationExternalProof(const BimManager* source,const container::scene::SceneProviderRegistry& registry,
      size_t materialCount,bool removal,std::optional<uint32_t> forbidden=std::nullopt,
      const container::scene::SceneManager* resourceOwner=nullptr):bim(source),resourceManager(resourceOwner) {
    if(resourceManager) {
      if(materialCount>1'000'000 || resourceManager->materialCount()!=materialCount)
        throw std::length_error("Resource consumer material identity proof exceeds its limit");
      resourceAsset=resourceManager->materialAssetIdentity();resourceMaterials=resourceManager->materialRevision();
      resourceAuthored=resourceManager->authoredMaterialRevision();materialIdentities.reserve(materialCount);
      for(uint32_t row=0;row<materialCount;++row) materialIdentities.push_back(resourceManager->materialResourceHandle(row));
    }
    const auto requireActiveMaterial=[&](uint32_t id,bool optional=false) {
      if(optional && id==UINT32_MAX) return;
      if(resourceManager && (id>=materialIdentities.size() || !materialIdentities[id] || !resourceManager->materialIsActive(id)))
        throw std::invalid_argument("A retained provider references an inactive material incarnation");
    };
    const auto current=registry.providers();
    if(current.size()>providers.size()) throw std::length_error("Material provider proof exceeds its limit");
    providerCount=current.size();std::copy(current.begin(),current.end(),providers.begin());
    if(bim) {
      available=bim->hasScene();geometry=bim->geometryRevision();objects=bim->objectDataRevision();metadata=bim->metadataRevision();
      buffer=bim->objectBuffer();
      const auto rows=bim->elementMetadata();
      if(bim->objectData().size()>1'000'000 || rows.size()>1'000'000)
        throw std::length_error("Material external-reference proof exceeds its limit");
      objectMaterials.reserve(bim->objectData().size());metadataMaterials.reserve(rows.size());
      for(const auto& object:bim->objectData()) {
        const auto id=object.objectInfo.x;
        requireActiveMaterial(id);
        if((removal && id>=materialCount) || forbidden==id) throw std::invalid_argument("A retained BIM object references the material");
        objectMaterials.push_back(id);
      }
      for(const auto row:rows) {
        const auto id=row.materialIndex;
        requireActiveMaterial(id,true);
        if((removal && id!=UINT32_MAX && id>=materialCount) || forbidden==id)
          throw std::invalid_argument("Retained BIM metadata references the material");
        metadataMaterials.push_back(id);
      }
    }
    providerStates.reserve(current.size());
    for(const auto* provider:current) {
      if(!provider) throw std::invalid_argument("A retained material provider is unavailable");
      const auto references=provider->materialReferenceView();
      if(!references || !references->complete)
        throw std::invalid_argument("A provider's material reference capability is incomplete");
      if(references->id.size()>65536 || references->displayName.size()>65536 || references->triangleBatches.size()>1'000'000)
        throw std::length_error("Material provider metadata exceeds its proof limit");
      const auto snapshot=provider->snapshot();
      const bool primary=dynamic_cast<const container::scene::MeshSceneProvider*>(provider) && snapshot.id.value=="primary-mesh-scene";
      const bool auxiliary=dynamic_cast<const container::scene::BimSceneProvider*>(provider) && snapshot.id.value=="auxiliary-bim-scene";
      const bool independent=(dynamic_cast<const container::scene::GaussianSplatSceneProvider*>(provider) && snapshot.id.value=="gaussian-splat-scene") ||
          (dynamic_cast<const container::scene::RadianceFieldSceneProvider*>(provider) && snapshot.id.value=="radiance-field-scene");
      if(!primary && !auxiliary && !independent)
        throw std::invalid_argument("A provider's material reference domain cannot be proved");
      if(snapshot.id.value.size()>65536 || snapshot.displayName.size()>65536 || snapshot.triangleBatches.size()>1'000'000)
        throw std::length_error("Material provider metadata exceeds its proof limit");
      if(!container::scene::SceneProviderMaterialReferenceViewMatches(*references,snapshot))
        throw std::invalid_argument("A provider changed while its material reference proof was captured");
      // Primary batches are replaced by the prospective binding-aware asset.
      if(resourceManager) for(const auto& batch:snapshot.triangleBatches) {
        if(batch.materialIndex< -1) throw std::invalid_argument("A provider has an invalid material sentinel");
        if(batch.materialIndex>=0) requireActiveMaterial(uint32_t(batch.materialIndex));
      }
      if(!primary) for(const auto& batch:snapshot.triangleBatches)
        if(batch.materialIndex>=0 && ((removal && size_t(batch.materialIndex)>=materialCount) || forbidden==uint32_t(batch.materialIndex)))
          throw std::invalid_argument("A retained provider references the material");
      providerStates.push_back(snapshot);
    }
  }
  bool matches(const container::scene::SceneProviderRegistry& registry) const noexcept {
    try {
    if(resourceManager) {
      if(resourceManager->materialAssetIdentity()!=resourceAsset || resourceManager->materialRevision()!=resourceMaterials ||
          resourceManager->authoredMaterialRevision()!=resourceAuthored || resourceManager->materialCount()!=materialIdentities.size()) return false;
      for(uint32_t row=0;row<materialIdentities.size();++row)
        if(resourceManager->materialResourceHandle(row)!=materialIdentities[row] ||
            bool(materialIdentities[row])!=resourceManager->materialIsActive(row)) return false;
    }
    const auto current=registry.providers();
    if(current.size()!=providerCount || !std::equal(current.begin(),current.end(),providers.begin())) return false;
    for(size_t i=0;i<providerStates.size();++i) {
      const auto now=current[i]->materialReferenceView();
      if(!now || !container::scene::SceneProviderMaterialReferenceViewMatches(*now,providerStates[i])) return false;
    }
    if(!bim) return true;
    if(available!=bim->hasScene() || geometry!=bim->geometryRevision() || objects!=bim->objectDataRevision() ||
        metadata!=bim->metadataRevision() || buffer!=bim->objectBuffer() || objectMaterials.size()!=bim->objectData().size()) return false;
    for(size_t i=0;i<objectMaterials.size();++i) if(objectMaterials[i]!=bim->objectData()[i].objectInfo.x) return false;
    const auto rows=bim->elementMetadata();if(rows.size()!=metadataMaterials.size()) return false;
    size_t i{};for(const auto row:rows) if(row.materialIndex!=metadataMaterials[i++]) return false;
    return true;
    } catch(...) {return false;}
  }
  uint64_t ownedBytes() const noexcept {
    uint64_t bytes=sizeof(*this)+objectMaterials.capacity()*sizeof(uint32_t)+metadataMaterials.capacity()*sizeof(uint32_t)+
        materialIdentities.capacity()*sizeof(container::util::ResourceHandle)+providerStates.capacity()*sizeof(container::scene::SceneProviderSnapshot);
    for(const auto& state:providerStates) bytes+=state.id.value.capacity()+state.displayName.capacity()+
        state.triangleBatches.size()*sizeof(container::scene::SceneProviderTriangleBatch);
    return bytes;
  }
};

} // namespace container::renderer::detail
