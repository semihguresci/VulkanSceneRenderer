#include "Container/utility/TextureSamplingPlan.h"

#include <algorithm>
#include <limits>
#include <stdexcept>

#include "Container/utility/AllocationManager.h"

namespace container::material {
namespace {
constexpr size_t maxSlots = 1'000'000;

TextureSamplingRowState snapshot(const TextureResource* resource) noexcept {
  if(!resource) return {};
  return {true,resource->allocationIdentity,resource->image,resource->imageView,
      resource->format,resource->samplerIndex,resource->width,resource->height,resource->mipLevels};
}

bool valid(const TextureResource& resource,const container::gpu::AllocationManager& allocation) noexcept {
  return resource.width && resource.height && resource.mipLevels && resource.format!=VK_FORMAT_UNDEFINED &&
      resource.samplerIndex<kMaterialSamplerKeyCount && allocation.textureImageMatches(resource);
}
}

uint64_t TextureSamplingPlanStorageBytes(size_t slots) {
  if(slots>maxSlots) throw std::length_error("Texture sampling table exceeds its finite row capacity");
  const uint64_t rows=std::max<size_t>(1,slots);
  return sizeof(TextureSamplingPlan)+rows*(sizeof(TextureSamplingRowState)+sizeof(VkDescriptorImageInfo)+
      sizeof(container::gpu::GpuTextureMetadata)+sizeof(MaterialSamplerKey))+
      uint64_t(kMaterialSamplerKeyCount)*sizeof(MaterialSamplerKey);
}

MaterialSamplerSlotPlan PlanTextureSamplerSlots(const TextureManager& textures,uint32_t capacity,
    std::span<const MaterialSamplerKey> additional) {
  if(textures.textureCount()>maxSlots || additional.size()>maxSlots-textures.textureCount())
    throw std::length_error("Texture sampler policy preflight exceeds its finite work capacity");
  if(!capacity || capacity>kMaterialSamplerKeyCount)
    throw std::invalid_argument("Texture sampler policy capacity is invalid");
  std::vector<MaterialSamplerKey> keys;
  keys.reserve(textures.textureCount()+additional.size());
  for(uint32_t index=0;index<textures.textureCount();++index) {
    const auto* resource=textures.getTexture(index);
    keys.push_back({resource?resource->samplerIndex:0});
  }
  keys.insert(keys.end(),additional.begin(),additional.end());
  return PlanMaterialSamplerSlots(keys,capacity);
}

TextureSamplingPlan MakeTextureSamplingPlan(const TextureManager& textures,const TextureResource& fallback,
    const container::gpu::AllocationManager& allocation,uint32_t imageCapacity,uint32_t samplerCapacity,
    uint64_t maxCpuBytes) {
  const auto bytes=TextureSamplingPlanStorageBytes(textures.textureCount());
  if(!maxCpuBytes || maxCpuBytes==UINT64_MAX || bytes>maxCpuBytes)
    throw std::length_error("Texture sampling preparation exceeds its finite CPU budget");
  if(!imageCapacity || textures.textureCount()>imageCapacity)
    throw std::length_error("Texture sampling table exceeds its stable descriptor capacity");
  if(!valid(fallback,allocation) || fallback.samplerIndex!=0 || fallback.width!=1 || fallback.height!=1 || fallback.mipLevels!=1)
    throw std::invalid_argument("Texture sampling fallback is unavailable or invalid");
  for(uint32_t index=0;index<textures.textureCount();++index)
    if(const auto* resource=textures.getTexture(index);resource && !valid(*resource,allocation))
      throw std::invalid_argument("Active texture sampling resource is unavailable or invalid");
  TextureSamplingPlan result;
  result.textureRevision=textures.revision();
  result.fallback=snapshot(&fallback);
  result.samplerSlots=PlanTextureSamplerSlots(textures,samplerCapacity);
  const auto count=std::max<size_t>(1,textures.textureCount());
  result.rows.reserve(textures.textureCount());result.images.reserve(count);result.metadata.reserve(count);
  for(uint32_t index=0;index<textures.textureCount();++index) {
    const auto* resource=textures.getTexture(index);
    result.rows.push_back(snapshot(resource));
    const auto& resolved=resource?*resource:fallback;
    result.images.push_back({VK_NULL_HANDLE,resolved.imageView,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
    container::gpu::GpuTextureMetadata row{};
    row.samplerIndex=result.samplerSlots.slot({resolved.samplerIndex});
    row.samplerCount=uint32_t(result.samplerSlots.keys.size());
    result.metadata.push_back(row);
  }
  if(result.images.empty()) {
    result.images.push_back({VK_NULL_HANDLE,fallback.imageView,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
    container::gpu::GpuTextureMetadata row{};
    row.samplerIndex=result.samplerSlots.slot({0});row.samplerCount=uint32_t(result.samplerSlots.keys.size());
    result.metadata.push_back(row);
  }
  return result;
}

namespace {
bool matches(const TextureSamplingPlan& plan,const TextureManager& textures,
    const TextureResource& fallback,const container::gpu::AllocationManager& allocation,bool prefix) noexcept {
  const auto published=plan.rows.size();
  if(published>textures.textureCount() || textures.textureCount()>maxSlots ||
      (!prefix && published!=textures.textureCount())) return false;
  const uint64_t appended=textures.textureCount()-published;
  if(plan.textureRevision>UINT64_MAX-appended || textures.revision()!=plan.textureRevision+appended) return false;
  const auto count=std::max<size_t>(1,published);
  if(plan.images.size()!=count || plan.metadata.size()!=count ||
      !(plan.fallback==snapshot(&fallback)) || !valid(fallback,allocation) || fallback.samplerIndex!=0 ||
      fallback.width!=1 || fallback.height!=1 || fallback.mipLevels!=1 ||
      plan.samplerSlots.keys.empty() || plan.samplerSlots.keys.size()>kMaterialSamplerKeyCount ||
      plan.samplerSlots.keys.front().value!=0 || plan.samplerSlots.slots[0]!=0) return false;
  std::array<bool,kMaterialSamplerKeyCount> seen{};seen[0]=true;
  uint32_t next=1;
  for(uint32_t index=0;index<published;++index) {
    const auto* resource=textures.getTexture(index);
    if(!(plan.rows[index]==snapshot(resource)) || (resource && !valid(*resource,allocation))) return false;
    const auto key=resource?resource->samplerIndex:0;
    if(!seen[key]) {
      if(next>=plan.samplerSlots.keys.size() || plan.samplerSlots.keys[next].value!=key ||
          plan.samplerSlots.slots[key]!=next) return false;
      seen[key]=true;++next;
    }
    const auto& image=plan.images[index];const auto& row=plan.metadata[index];
    if(image.sampler!=VK_NULL_HANDLE || image.imageView!=(resource?resource->imageView:fallback.imageView) ||
        image.imageLayout!=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL ||
        row.samplerIndex!=plan.samplerSlots.slots[key] || row.samplerCount!=plan.samplerSlots.keys.size() ||
        row.padding1 || row.padding2) return false;
  }
  if(next!=plan.samplerSlots.keys.size()) return false;
  for(size_t key=0;key<seen.size();++key)
    if(!seen[key] && plan.samplerSlots.slots[key]!=kInvalidMaterialSamplerSlot) return false;
  if(published==0) {
    const auto& image=plan.images.front();const auto& row=plan.metadata.front();
    if(image.sampler!=VK_NULL_HANDLE || image.imageView!=fallback.imageView ||
        image.imageLayout!=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL || row.samplerIndex!=0 ||
        row.samplerCount!=1 || row.padding1 || row.padding2) return false;
  }
  for(uint32_t index=uint32_t(published);index<textures.textureCount();++index) {
    const auto* resource=textures.getTexture(index);
    if(!resource || !valid(*resource,allocation)) return false;
  }
  return true;
}
} // namespace

bool TextureSamplingPlanMatches(const TextureSamplingPlan& plan,const TextureManager& textures,
    const TextureResource& fallback,const container::gpu::AllocationManager& allocation) noexcept {
  return matches(plan,textures,fallback,allocation,false);
}

bool TextureSamplingPlanPrefixMatches(const TextureSamplingPlan& plan,const TextureManager& textures,
    const TextureResource& fallback,const container::gpu::AllocationManager& allocation) noexcept {
  return matches(plan,textures,fallback,allocation,true);
}

} // namespace container::material
