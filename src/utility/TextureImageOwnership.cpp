#include "Container/utility/TextureImageOwnership.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace container::material {
namespace {
constexpr size_t maxRows=1'000'000;
constexpr size_t maxSets=16;
TextureImageCapability capability(const TextureResource& resource) noexcept {
  return {resource.allocationIdentity,resource.image,resource.imageView};
}
TextureResource resource(const TextureImageCapability& image) noexcept {
  TextureResource result;result.allocationIdentity=image.identity;result.image=image.image;result.imageView=image.imageView;
  return result;
}
void budget(size_t rows,uint64_t bytes) {
  if(!bytes || bytes==UINT64_MAX || TextureImageOwnershipStorageBytes(rows)>bytes)
    throw std::length_error("Texture image ownership exceeds its finite CPU budget");
}
}

struct TextureImageOwnershipAccess {
  static const container::gpu::AllocationManager* owner(const TextureImageOwnership& set) noexcept {return set.owner_;}
  static TextureImageOwnershipPtr make(const container::gpu::AllocationManager& owner,uint64_t revision,size_t slots,bool complete,
      std::vector<TextureImageCapability> images,std::vector<TextureImagePin> pins,std::vector<TextureImageOwnershipRow> rows) {
    auto result=std::shared_ptr<TextureImageOwnership>(new TextureImageOwnership);
    result->owner_=&owner;result->revision_=revision;result->slots_=slots;result->complete_=complete;
    result->images_=std::move(images);result->pins_=std::move(pins);result->rows_=std::move(rows);return result;
  }
  static TextureImageOwnershipAdoption adoption(TextureImageOwnershipPtr ownership,std::vector<TextureImagePin> pins) {
    TextureImageOwnershipAdoption result;result.ownership_=std::move(ownership);result.pins_=std::move(pins);return result;
  }
  static auto pins(const TextureImageOwnershipAdoption& adoption) noexcept {return std::span<const TextureImagePin>(adoption.pins_);}
  static bool consumed(const TextureImageOwnershipAdoption& adoption) noexcept {return adoption.consumed_;}
  static void consume(TextureImageOwnershipAdoption& adoption) noexcept {adoption.consumed_=true;}
};

uint64_t TextureImageOwnership::ownedBytes() const noexcept {
  return sizeof(*this)+128+uint64_t(images_.capacity())*sizeof(TextureImageCapability)+
      uint64_t(pins_.capacity())*(sizeof(TextureImagePin)+128)+uint64_t(rows_.capacity())*sizeof(TextureImageOwnershipRow);
}

uint64_t TextureImageOwnershipStorageBytes(size_t rows) {
  if(rows>maxRows) throw std::length_error("Texture image ownership exceeds its finite row capacity");
  // Object/control, retained rows/pins/capabilities and simultaneous sorting
  // scratch; lease/control blocks are charged even when inherited pins reuse them.
  return sizeof(TextureImageOwnership)+256+uint64_t(rows)*(sizeof(TextureImageCapability)+sizeof(TextureImagePin)+
      sizeof(TextureImageOwnershipRow)+128+64);
}

bool TextureImageOwnershipValid(const TextureImageOwnershipPtr& set,const container::gpu::AllocationManager& allocation) noexcept {
  if(!set || TextureImageOwnershipAccess::owner(*set)!=&allocation || set->slotCount()>maxRows ||
      set->rows().size()>maxRows || set->imageCount()>set->rows().size() || set->imageCount()!=set->pins().size()) return false;
  if(set->complete() && set->rows().size()!=set->slotCount()) return false;
  uint64_t previous{};
  for(size_t index=0;index<set->imageCount();++index) {
    const auto& image=set->images()[index];const auto& pin=set->pins()[index];
    if(!pin || !image.identity || image.identity.incarnation()<=previous || !(pin->identity()==image.identity) ||
        !allocation.textureImageMatches(resource(image))) return false;
    previous=image.identity.incarnation();
  }
  uint32_t priorSlot{};uint64_t priorImage{};bool first=true;
  for(size_t index=0;index<set->rows().size();++index) {
    const auto& row=set->rows()[index];
    if(row.runtimeSlot>=set->slotCount() || (set->complete() && row.runtimeSlot!=index) ||
        (row.image!=UINT32_MAX && row.image>=set->imageCount()) || (!set->complete() && row.image==UINT32_MAX)) return false;
    const auto incarnation=row.image==UINT32_MAX?0:set->images()[row.image].identity.incarnation();
    if(!first && (row.runtimeSlot<priorSlot || (row.runtimeSlot==priorSlot && incarnation<=priorImage))) return false;
    priorSlot=row.runtimeSlot;priorImage=incarnation;first=false;
  }
  return true;
}

bool TextureImageOwnershipMatches(const TextureImageOwnershipPtr& set,const TextureManager& table,
    const container::gpu::AllocationManager& allocation) noexcept {
  if(!TextureImageOwnershipValid(set,allocation) || !set->complete() || set->revision()!=table.revision() ||
      set->slotCount()!=table.textureCount()) return false;
  for(uint32_t slot=0;slot<table.textureCount();++slot) {
    const auto* row=table.getTexture(slot);const auto image=set->rows()[slot].image;
    if(!row) {if(image!=UINT32_MAX) return false;}
    else if(image==UINT32_MAX || !(set->images()[image]==capability(*row))) return false;
  }
  return true;
}

bool TextureImageOwnershipContains(const TextureImageOwnershipPtr& set,const TextureResource& row,
    const container::gpu::AllocationManager& allocation) noexcept {
  if(!TextureImageOwnershipValid(set,allocation) || !allocation.textureImageMatches(row)) return false;
  const auto images=set->images();const auto incarnation=row.allocationIdentity.incarnation();
  const auto found=std::lower_bound(images.begin(),images.end(),incarnation,
      [](const auto& image,uint64_t id){return image.identity.incarnation()<id;});
  return found!=images.end() && *found==capability(row);
}

TextureImageOwnershipPtr PrepareTextureImageOwnership(const TextureManager& table,container::gpu::AllocationManager& allocation,
    std::span<const TextureImageOwnershipPtr> inherited,uint64_t maxCpuBytes) {
  budget(table.textureCount(),maxCpuBytes);
  if(inherited.size()>maxSets) throw std::length_error("Too many inherited texture ownership generations");
  for(const auto& set:inherited) {
    if(!set) continue;
    if(!TextureImageOwnershipValid(set,allocation)) throw std::invalid_argument("Inherited texture image ownership is stale or foreign");
  }
  for(const auto& set:inherited) if(set && TextureImageOwnershipMatches(set,table,allocation)) return set;
  size_t active{};
  for(uint32_t slot=0;slot<table.textureCount();++slot) if(const auto* row=table.getTexture(slot)) {
    if(!allocation.textureImageMatches(*row)) throw std::invalid_argument("Active image has an invalid allocation capability");
    ++active;
  }
  struct Input {const TextureResource* resource{};uint32_t slot{};};
  std::vector<Input> inputs;inputs.reserve(active);
  std::vector<TextureImageOwnershipRow> rows;rows.reserve(table.textureCount());
  for(uint32_t slot=0;slot<table.textureCount();++slot) {
    rows.push_back({slot,UINT32_MAX});if(const auto* row=table.getTexture(slot)) inputs.push_back({row,slot});
  }
  std::sort(inputs.begin(),inputs.end(),[](const auto& a,const auto& b) {
    const auto first=a.resource->allocationIdentity.incarnation(),second=b.resource->allocationIdentity.incarnation();
    return first==second?a.slot<b.slot:first<second;
  });
  std::vector<TextureImageCapability> images;images.reserve(active);
  std::vector<TextureImagePin> pins;pins.reserve(active);
  for(size_t at=0;at<inputs.size();) {
    const auto& image=*inputs[at].resource;const auto exact=capability(image);TextureImagePin pin;
    for(const auto& source:inherited) {
      if(!source) continue;const auto known=source->images();
      const auto found=std::lower_bound(known.begin(),known.end(),image.allocationIdentity.incarnation(),
          [](const auto& candidate,uint64_t id){return candidate.identity.incarnation()<id;});
      if(found!=known.end() && *found==exact) {pin=source->pins()[size_t(found-known.begin())];break;}
    }
    if(!pin) pin=allocation.retainTextureImage(image);
    const auto index=uint32_t(images.size());images.push_back(exact);pins.push_back(std::move(pin));
    do {rows[inputs[at].slot].image=index;++at;}
    while(at<inputs.size() && inputs[at].resource->allocationIdentity==image.allocationIdentity);
  }
  return TextureImageOwnershipAccess::make(allocation,table.revision(),table.textureCount(),true,
      std::move(images),std::move(pins),std::move(rows));
}

TextureImageOwnershipPtr SelectTextureImageOwnership(const TextureImageOwnershipPtr& source,std::span<const uint32_t> runtimeSlots,
    const container::gpu::AllocationManager& allocation,uint64_t maxCpuBytes) {
  budget(runtimeSlots.size(),maxCpuBytes);
  if(!TextureImageOwnershipValid(source,allocation) || !source->complete())
    throw std::invalid_argument("Selected image retention requires a complete valid generation");
  std::vector<uint32_t> slots;slots.reserve(runtimeSlots.size());
  for(const auto slot:runtimeSlots) if(slot!=UINT32_MAX) {
    if(slot>=source->slotCount() || source->rows()[slot].image==UINT32_MAX)
      throw std::invalid_argument("Selected material image slot is retired or outside its generation");
    slots.push_back(slot);
  }
  std::sort(slots.begin(),slots.end());slots.erase(std::unique(slots.begin(),slots.end()),slots.end());
  struct Input {uint32_t row{},sourceImage{};};
  std::vector<Input> inputs;inputs.reserve(slots.size());
  std::vector<TextureImageOwnershipRow> rows;rows.reserve(slots.size());
  for(const auto slot:slots) {inputs.push_back({uint32_t(rows.size()),source->rows()[slot].image});rows.push_back({slot,UINT32_MAX});}
  std::sort(inputs.begin(),inputs.end(),[](const auto& a,const auto& b){return a.sourceImage<b.sourceImage;});
  std::vector<TextureImageCapability> images;images.reserve(slots.size());
  std::vector<TextureImagePin> pins;pins.reserve(slots.size());
  for(size_t at=0;at<inputs.size();) {
    const auto sourceImage=inputs[at].sourceImage;const auto index=uint32_t(images.size());
    images.push_back(source->images()[sourceImage]);pins.push_back(source->pins()[sourceImage]);
    do {rows[inputs[at].row].image=index;++at;} while(at<inputs.size() && inputs[at].sourceImage==sourceImage);
  }
  return TextureImageOwnershipAccess::make(allocation,source->revision(),source->slotCount(),false,
      std::move(images),std::move(pins),std::move(rows));
}

TextureImageOwnershipPtr CombineTextureImageOwnership(std::span<const TextureImageOwnershipPtr> sources,
    const container::gpu::AllocationManager& allocation,uint64_t maxCpuBytes) {
  if(sources.size()>maxSets) throw std::length_error("Too many selected image ownership generations");
  size_t count{},slots{};
  for(const auto& source:sources) {
    if(!TextureImageOwnershipValid(source,allocation)) throw std::invalid_argument("Selected image union has a stale or foreign generation");
    if(source->rows().size()>maxRows-count) throw std::length_error("Selected image union exceeds its row bound");
    count+=source->rows().size();slots=std::max(slots,source->slotCount());
  }
  budget(count,maxCpuBytes);
  struct Input {uint32_t slot{};TextureImageCapability image;TextureImagePin pin;};
  std::vector<Input> inputs;inputs.reserve(count);
  for(const auto& source:sources) for(const auto& row:source->rows()) if(row.image!=UINT32_MAX)
    inputs.push_back({row.runtimeSlot,source->images()[row.image],source->pins()[row.image]});
  std::sort(inputs.begin(),inputs.end(),[](const auto& a,const auto& b) {
    const auto first=a.image.identity.incarnation(),second=b.image.identity.incarnation();
    return first==second?a.slot<b.slot:first<second;
  });
  std::vector<TextureImageCapability> images;images.reserve(inputs.size());
  std::vector<TextureImagePin> pins;pins.reserve(inputs.size());
  std::vector<TextureImageOwnershipRow> rows;rows.reserve(inputs.size());
  for(const auto& input:inputs) {
    if(images.empty() || !(images.back()==input.image)) {images.push_back(input.image);pins.push_back(input.pin);}
    rows.push_back({input.slot,uint32_t(images.size()-1)});
  }
  std::sort(rows.begin(),rows.end(),[](const auto& a,const auto& b){return a.runtimeSlot==b.runtimeSlot?a.image<b.image:a.runtimeSlot<b.runtimeSlot;});
  rows.erase(std::unique(rows.begin(),rows.end()),rows.end());
  return TextureImageOwnershipAccess::make(allocation,0,slots,false,std::move(images),std::move(pins),std::move(rows));
}

TextureImageOwnershipAdoption PrepareTextureImageOwnershipAdoption(const TextureImageOwnershipPtr& ownership,
    const TextureManager& table,const container::gpu::AllocationManager& allocation,uint64_t maxCpuBytes) {
  if(!TextureImageOwnershipMatches(ownership,table,allocation)) throw std::invalid_argument("Image adoption requires complete current ownership");
  budget(ownership->imageCount(),maxCpuBytes);
  size_t pending{};
  for(const auto& pin:ownership->pins()) if(allocation.canAdoptTextureImages(std::span(&pin,1))) ++pending;
  std::vector<TextureImagePin> pins;pins.reserve(pending);
  for(const auto& pin:ownership->pins())
    if(allocation.canAdoptTextureImages(std::span(&pin,1))) pins.push_back(pin);
  if(!pins.empty() && !allocation.canAdoptTextureImages(pins)) throw std::invalid_argument("Image adoption group-zero set is stale or incomplete");
  return TextureImageOwnershipAccess::adoption(ownership,std::move(pins));
}

bool CanAdoptTextureImageOwnership(const TextureImageOwnershipAdoption& adoption,const TextureManager& table,
    const container::gpu::AllocationManager& allocation) noexcept {
  if(!adoption || TextureImageOwnershipAccess::consumed(adoption) || !TextureImageOwnershipMatches(adoption.ownership(),table,allocation)) return false;
  const auto selected=TextureImageOwnershipAccess::pins(adoption);size_t next{};
  for(const auto& pin:adoption.ownership()->pins()) if(allocation.canAdoptTextureImages(std::span(&pin,1))) {
    if(next>=selected.size() || selected[next++]!=pin) return false;
  }
  return next==selected.size() && (selected.empty() || allocation.canAdoptTextureImages(selected));
}

bool AdoptTextureImageOwnership(TextureImageOwnershipAdoption& adoption,const TextureManager& table,
    container::gpu::AllocationManager& allocation) noexcept {
  if(!CanAdoptTextureImageOwnership(adoption,table,allocation)) return false;
  const auto pins=TextureImageOwnershipAccess::pins(adoption);
  if(!pins.empty() && !allocation.adoptTextureImages(pins)) return false;
  TextureImageOwnershipAccess::consume(adoption);return true;
}

} // namespace container::material
