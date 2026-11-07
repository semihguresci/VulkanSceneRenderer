#include "Container/utility/TextureResourceLifecycle.h"
#include "Container/geometry/GltfTextureResourceReferences.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace container::scene {
namespace {
using namespace container::util;
using namespace container::geometry::gltf;
using Kind=ResourceKind;
using Domain=ResourceDomainKind;
using Pins=ResourcePinClass;
constexpr size_t tableCount=size_t(Domain::Count)*size_t(Kind::Count);
constexpr uint64_t ownerWorkLimit=64'000'000;
[[noreturn]] void invalid(const char* text) {throw std::invalid_argument(text);}
[[noreturn]] void exceeded() {throw std::length_error("Texture resource lifecycle exceeds its finite budget");}
uint64_t plus(uint64_t a,uint64_t b) {if(b>UINT64_MAX-a) exceeded();return a+b;}
uint64_t times(uint64_t a,uint64_t b) {if(a && b>UINT64_MAX/a) exceeded();return a*b;}
bool supported(Domain domain,Kind kind) noexcept {
  return ((domain==Domain::Archive || domain==Domain::Evaluated) &&
      (kind==Kind::Image || kind==Kind::Texture || kind==Kind::Sampler)) ||
      (domain==Domain::Runtime && kind==Kind::RuntimeTexture);
}
size_t table(Domain domain,Kind kind) noexcept {return size_t(domain)*size_t(Kind::Count)+size_t(kind);}
struct Budget {
  TextureResourceLifecycleLimits limits;
  uint64_t bytes{},work{};
  explicit Budget(TextureResourceLifecycleLimits value):limits(value) {
    if(!value.maxRows || value.maxRows>UINT32_MAX || !value.maxOwnedBytes || value.maxOwnedBytes==UINT64_MAX ||
        !value.maxWork || value.maxWork==UINT64_MAX ||
        !value.maxLiveOwners || value.maxLiveOwners==UINT64_MAX || !value.maxLiveReferences || value.maxLiveReferences==UINT64_MAX ||
        (value.retirementPolicy!=ResourceRetirementPolicy::BlockPreparations &&
         value.retirementPolicy!=ResourceRetirementPolicy::InvalidatePreparations)) exceeded();
  }
  void charge(uint64_t n) {bytes=plus(bytes,n);if(bytes>limits.maxOwnedBytes) exceeded();}
  void visit(uint64_t n=1) {work=plus(work,n);if(work>limits.maxWork) exceeded();}
  void string(const std::string& s) {charge(plus(s.size(),1));visit(s.size());}
};
struct Check {
  uint64_t remaining;
  bool take(uint64_t n=1) noexcept {if(n>remaining) return false;remaining-=n;return true;}
};
void valueCost(const tinygltf::Value& value,Budget& budget,unsigned depth=0) {
  if(depth>64) exceeded();budget.visit();budget.charge(sizeof(tinygltf::Value)+64);
  if(value.IsString()) budget.string(value.Get<std::string>());
  else if(value.IsBinary()) {const auto n=value.Get<std::vector<unsigned char>>().size();budget.charge(n);budget.visit(n);}
  else if(value.IsArray()) for(const auto& v:value.Get<tinygltf::Value::Array>()) valueCost(v,budget,depth+1);
  else if(value.IsObject()) for(const auto& [key,v]:value.Get<tinygltf::Value::Object>()) {budget.string(key);valueCost(v,budget,depth+1);}
}
bool sameString(const std::string& a,const std::string& b,Check& check) noexcept {
  return a.size()==b.size() && check.take(a.size()) && a==b;
}
bool sameValue(const tinygltf::Value& a,const tinygltf::Value& b,Check& check,unsigned depth=0) noexcept {
  if(depth>64 || !check.take() || a.Type()!=b.Type()) return false;
  if(a.IsString()) return sameString(a.Get<std::string>(),b.Get<std::string>(),check);
  if(a.IsBool()) return a.Get<bool>()==b.Get<bool>();
  if(a.IsInt()) return a.Get<int>()==b.Get<int>();
  if(a.IsReal()) return std::bit_cast<uint64_t>(a.Get<double>())==std::bit_cast<uint64_t>(b.Get<double>());
  if(a.IsBinary()) {
    const auto& x=a.Get<std::vector<unsigned char>>();const auto& y=b.Get<std::vector<unsigned char>>();
    return x.size()==y.size() && check.take(x.size()) && x==y;
  }
  if(a.IsArray()) {
    const auto& x=a.Get<tinygltf::Value::Array>();const auto& y=b.Get<tinygltf::Value::Array>();
    if(x.size()!=y.size() || !check.take(x.size())) return false;
    for(size_t i=0;i<x.size();++i) if(!sameValue(x[i],y[i],check,depth+1)) return false;
  } else if(a.IsObject()) {
    const auto& x=a.Get<tinygltf::Value::Object>();const auto& y=b.Get<tinygltf::Value::Object>();
    if(x.size()!=y.size() || !check.take(x.size())) return false;
    auto j=y.begin();for(const auto& [key,v]:x) {if(!sameString(key,j->first,check) || !sameValue(v,j->second,check,depth+1)) return false;++j;}
  }
  return true;
}
template<class T> void metadataCost(const T& row,Budget& budget) {
  budget.charge(sizeof(T)+128);budget.string(row.name);budget.string(row.extras_json_string);budget.string(row.extensions_json_string);
  valueCost(row.extras,budget);
  for(const auto& [key,value]:row.extensions) {budget.charge(64);budget.string(key);valueCost(value,budget);}
}
template<class T> bool sameMetadata(const T& a,const T& b,Check& check) noexcept {
  if(!sameString(a.name,b.name,check) || !sameString(a.extras_json_string,b.extras_json_string,check) ||
      !sameString(a.extensions_json_string,b.extensions_json_string,check) || !sameValue(a.extras,b.extras,check) ||
      a.extensions.size()!=b.extensions.size() || !check.take(a.extensions.size())) return false;
  auto j=b.extensions.begin();for(const auto& [key,value]:a.extensions) {
    if(!sameString(key,j->first,check) || !sameValue(value,j->second,check)) return false;++j;
  }
  return true;
}
bool sameImage(const tinygltf::Image& a,const tinygltf::Image& b,Check& check) noexcept {
  return a.width==b.width && a.height==b.height && a.component==b.component && a.bits==b.bits &&
      a.pixel_type==b.pixel_type && a.bufferView==b.bufferView && a.as_is==b.as_is &&
      sameMetadata(a,b,check) && sameString(a.mimeType,b.mimeType,check) && sameString(a.uri,b.uri,check) &&
      a.image.size()==b.image.size() && check.take(a.image.size()) && a.image==b.image;
}
bool sameImageCore(const tinygltf::Image& a,const tinygltf::Image& b,Check& check) noexcept {
  return a.width==b.width && a.height==b.height && a.component==b.component && a.bits==b.bits &&
      a.pixel_type==b.pixel_type && a.bufferView==b.bufferView && a.as_is==b.as_is &&
      sameString(a.mimeType,b.mimeType,check) && sameString(a.uri,b.uri,check) &&
      a.image.size()==b.image.size() && check.take(a.image.size()) && a.image==b.image;
}
bool sameTexture(const tinygltf::Texture& a,const tinygltf::Texture& b,Check& check) noexcept {
  return a.source==b.source && a.sampler==b.sampler && sameMetadata(a,b,check);
}
bool sameSampler(const tinygltf::Sampler& a,const tinygltf::Sampler& b,Check& check) noexcept {
  return a.minFilter==b.minFilter && a.magFilter==b.magFilter && a.wrapS==b.wrapS && a.wrapT==b.wrapT && sameMetadata(a,b,check);
}
bool sameView(const tinygltf::BufferView& a,const tinygltf::BufferView& b,Check& check) noexcept {
  return a.buffer==b.buffer && a.byteOffset==b.byteOffset && a.byteLength==b.byteLength && a.byteStride==b.byteStride &&
      a.target==b.target && a.dracoDecoded==b.dracoDecoded && sameMetadata(a,b,check);
}
bool sameViewCore(const tinygltf::BufferView& a,const tinygltf::BufferView& b) noexcept {
  return a.buffer==b.buffer && a.byteOffset==b.byteOffset && a.byteLength==b.byteLength && a.byteStride==b.byteStride &&
      a.target==b.target && a.dracoDecoded==b.dracoDecoded;
}
bool sameRuntime(const container::material::TextureResource& a,const container::material::TextureResource& b,Check& check) noexcept {
  return a.image==b.image && a.imageView==b.imageView && a.sampler==b.sampler && a.samplerIndex==b.samplerIndex &&
      a.format==b.format && a.width==b.width && a.height==b.height && a.mipLevels==b.mipLevels &&
      a.allocationIdentity==b.allocationIdentity && sameString(a.name,b.name,check) && sameString(a.contentKey,b.contentKey,check);
}
using LiveLabel=std::tuple<uint64_t,ResourceKind,uint32_t,uint64_t,TextureResourceLiveOwnerKind>;
LiveLabel liveLabel(TextureResourceLiveOwnerKey key) noexcept {
  return {key.owner.domain().identity(),key.owner.kind(),key.owner.slot(),key.owner.incarnation(),key.kind};
}
struct LiveSlot {std::weak_ptr<const TextureResourceLiveOwner> active;uint64_t reservations{};};
struct Authority {
  std::shared_ptr<ResourceLifecycleCatalog> catalog;
  std::array<ResourceLifecycleDomain,3> domains;
  std::array<uint32_t,tableCount> high{};
  uint64_t opaque{},maxOpaque{};
  uint64_t liveVersion{};
  uint64_t liveOwners{},liveReferences{},maxLiveOwners{},maxLiveReferences{};
  std::map<LiveLabel,LiveSlot> liveSlots;
};
struct OpaqueBlocker {
  std::shared_ptr<Authority> authority;
  bool valid{true},counted{};
  explicit OpaqueBlocker(std::shared_ptr<Authority> value):authority(std::move(value)) {
    if(authority->opaque>=authority->maxOpaque) exceeded();++authority->opaque;counted=true;
  }
  void cancel() noexcept {valid=false;if(counted) {--authority->opaque;counted=false;}}
  ~OpaqueBlocker() {if(counted) --authority->opaque;}
};
struct Payload {
  std::variant<std::monostate,tinygltf::Image,tinygltf::Texture,tinygltf::Sampler,container::material::TextureResource> record;
  std::optional<tinygltf::BufferView> bufferView;
  std::vector<unsigned char> slice;
  GltfTextureRawResourceReferences dependencies;
  uint64_t bytes{};
  bool opaque{};
};
struct Row {ResourceHandle handle;std::shared_ptr<const Payload> payload;bool active{true};};
using Tables=std::array<std::vector<Row>,tableCount>;
struct PinControl {ResourcePin pin;explicit PinControl(ResourcePin&& value):pin(std::move(value)) {}};
struct SelectedRow {std::shared_ptr<Authority> authority;Row row;};
struct Prefix {size_t count{};ResourceHandle last;};
const TextureResourceSourceTables& source(const TextureResourceLifecycleView& view,Domain domain) noexcept {
  return domain==Domain::Archive?view.archive:view.evaluated;
}
size_t count(const TextureResourceLifecycleView& view,Domain domain,Kind kind) noexcept {
  if(domain==Domain::Runtime) return kind==Kind::RuntimeTexture?view.runtime.textureCount():0;
  const auto& s=source(view,domain);
  if(kind==Kind::Image) return s.images.size();if(kind==Kind::Texture) return s.textures.size();if(kind==Kind::Sampler) return s.samplers.size();return 0;
}
std::span<const uint8_t> sourceMask(const TextureResourceSourceTables& source,Kind kind) noexcept {
  if(kind==Kind::Image) return source.imageActive;
  if(kind==Kind::Texture) return source.textureActive;
  if(kind==Kind::Sampler) return source.samplerActive;
  return {};
}
bool masksValid(const TextureResourceLifecycleView& view,Check& check) noexcept {
  for(const auto domain:{Domain::Archive,Domain::Evaluated}) for(const auto kind:{Kind::Image,Kind::Texture,Kind::Sampler}) {
    const auto mask=sourceMask(source(view,domain),kind);
    if(!mask.empty() && mask.size()!=count(view,domain,kind)) return false;
    if(!check.take(mask.size())) return false;
    for(const auto active:mask) if(active>1) return false;
  }
  return true;
}
bool viewActive(const TextureResourceLifecycleView& view,Domain domain,Kind kind,size_t index) noexcept {
  if(domain==Domain::Runtime) return view.runtime.isTextureActive(uint32_t(index));
  const auto mask=sourceMask(source(view,domain),kind);
  return mask.empty() || (index<mask.size() && mask[index]!=0);
}
std::span<const unsigned char> imageSlice(const TextureResourceSourceTables& source,const tinygltf::Image& image) {
  if(image.bufferView<0) return {};
  if(size_t(image.bufferView)>=source.bufferViews.size()) invalid("Image bufferView is missing");
  const auto& view=source.bufferViews[size_t(image.bufferView)];
  if(view.buffer<0 || size_t(view.buffer)>=source.buffers.size()) invalid("Image buffer is missing");
  const auto& data=source.buffers[size_t(view.buffer)].data;
  if(view.byteOffset>data.size() || view.byteLength>data.size()-view.byteOffset) invalid("Image bufferView range is invalid");
  return std::span<const unsigned char>(data).subspan(view.byteOffset,view.byteLength);
}
std::shared_ptr<const Payload> makePayload(const TextureResourceLifecycleView& view,Domain domain,Kind kind,size_t index,Budget& budget) {
  const auto start=budget.bytes;budget.charge(sizeof(Payload)+128);budget.visit();
  const auto& s=source(view,domain);
  if(kind==Kind::Image) {
    const auto& image=s.images[index];metadataCost(image,budget);budget.string(image.mimeType);budget.string(image.uri);
    budget.charge(image.image.size());budget.visit(image.image.size());const auto slice=imageSlice(s,image);
    budget.charge(slice.size());budget.visit(slice.size());
    if(image.bufferView>=0) metadataCost(s.bufferViews[size_t(image.bufferView)],budget);
    auto result=std::make_shared<Payload>();result->record.emplace<tinygltf::Image>(image);
    if(image.bufferView>=0) result->bufferView=s.bufferViews[size_t(image.bufferView)];
    // No Image/Sampler/bufferView extension-reference schema is proved by the
    // selected texture extractor. Preserve known payloads, but never advertise
    // complete reference authority for those normative extension objects.
    result->opaque=!image.extensions.empty() || (result->bufferView && !result->bufferView->extensions.empty());
    result->slice.assign(slice.begin(),slice.end());result->bytes=budget.bytes-start;return result;
  }
  if(kind==Kind::Texture) {
    const auto& texture=s.textures[index];metadataCost(texture,budget);
    GltfTextureResourceReferenceLimits limits;limits.policy=GltfTextureResourceReferencePolicy::RecordIncomplete;
    limits.maxTableEntries=budget.limits.maxRows;
    limits.maxOwnedBytes=budget.limits.maxOwnedBytes-budget.bytes;
    limits.maxTraversalSteps=budget.limits.maxWork-budget.work;
    if(!limits.maxOwnedBytes || !limits.maxTraversalSteps) exceeded();
    // Source masks are validated once per view; checking the full mask inside
    // every row extractor would make sparse-phase scenes quadratic. Exact
    // outgoing targets are phase-checked against the generation tables below.
    auto refs=ExtractGltfTextureRawResourceReferences(texture,s.images.size(),s.samplers.size(),{},{},limits);
    budget.charge(refs.stats().ownedBytes);budget.visit(refs.stats().traversalSteps);
    auto result=std::make_shared<Payload>();result->record.emplace<tinygltf::Texture>(texture);result->dependencies=std::move(refs);
    result->opaque=!result->dependencies.complete();
    result->bytes=budget.bytes-start;return result;
  }
  if(kind==Kind::Sampler) {
    const auto& sampler=s.samplers[index];metadataCost(sampler,budget);
    auto result=std::make_shared<Payload>();result->record.emplace<tinygltf::Sampler>(sampler);
    result->opaque=!sampler.extensions.empty();result->bytes=budget.bytes-start;return result;
  }
  const auto* runtime=view.runtime.getTexture(uint32_t(index));if(!runtime) return {};
  budget.charge(sizeof(*runtime));budget.string(runtime->name);budget.string(runtime->contentKey);
  auto result=std::make_shared<Payload>();result->record.emplace<container::material::TextureResource>(*runtime);result->bytes=budget.bytes-start;return result;
}
bool payloadMatches(const Row& row,const TextureResourceLifecycleView& view,Domain domain,Kind kind,size_t index,Check& check) noexcept {
  if(!check.take()) return false;
  if(domain==Domain::Runtime) {
    const auto* runtime=view.runtime.getTexture(uint32_t(index));
    return row.active?(runtime && row.payload && sameRuntime(std::get<container::material::TextureResource>(row.payload->record),*runtime,check)):!runtime;
  }
  if(row.active!=viewActive(view,domain,kind,index) || !row.payload) return false;const auto& s=source(view,domain);
  if(kind==Kind::Texture) return sameTexture(std::get<tinygltf::Texture>(row.payload->record),s.textures[index],check);
  if(kind==Kind::Sampler) return sameSampler(std::get<tinygltf::Sampler>(row.payload->record),s.samplers[index],check);
  const auto& image=s.images[index];if(!sameImage(std::get<tinygltf::Image>(row.payload->record),image,check)) return false;
  if(image.bufferView<0) return !row.payload->bufferView && row.payload->slice.empty();
  if(!row.payload->bufferView || size_t(image.bufferView)>=s.bufferViews.size()) return false;
  const auto& v=s.bufferViews[size_t(image.bufferView)];if(!sameView(*row.payload->bufferView,v,check) || v.buffer<0 || size_t(v.buffer)>=s.buffers.size()) return false;
  const auto& bytes=s.buffers[size_t(v.buffer)].data;
  return v.byteOffset<=bytes.size() && v.byteLength<=bytes.size()-v.byteOffset && v.byteLength==row.payload->slice.size() &&
      check.take(v.byteLength) && std::equal(row.payload->slice.begin(),row.payload->slice.end(),bytes.begin()+v.byteOffset);
}
bool corePayloadMatches(const Row& row,const TextureResourceLifecycleView& view,Domain domain,Kind kind,size_t index,Check& check) noexcept {
  if(domain==Domain::Runtime || !row.active || !row.payload || !check.take()) return false;
  const auto& s=source(view,domain);
  if(kind==Kind::Texture) {
    const auto& old=std::get<tinygltf::Texture>(row.payload->record);const auto& current=s.textures[index];
    return old.source==current.source && old.sampler==current.sampler;
  }
  if(kind==Kind::Sampler) {
    const auto& old=std::get<tinygltf::Sampler>(row.payload->record);const auto& current=s.samplers[index];
    return old.minFilter==current.minFilter && old.magFilter==current.magFilter && old.wrapS==current.wrapS && old.wrapT==current.wrapT;
  }
  const auto& image=s.images[index];if(!sameImageCore(std::get<tinygltf::Image>(row.payload->record),image,check)) return false;
  if(image.bufferView<0) return !row.payload->bufferView && row.payload->slice.empty();
  if(!row.payload->bufferView || size_t(image.bufferView)>=s.bufferViews.size()) return false;
  const auto& v=s.bufferViews[size_t(image.bufferView)];
  if(!sameViewCore(*row.payload->bufferView,v) || v.buffer<0 || size_t(v.buffer)>=s.buffers.size()) return false;
  const auto& bytes=s.buffers[size_t(v.buffer)].data;
  return v.byteOffset<=bytes.size() && v.byteLength<=bytes.size()-v.byteOffset && v.byteLength==row.payload->slice.size() &&
      check.take(v.byteLength) && std::equal(row.payload->slice.begin(),row.payload->slice.end(),bytes.begin()+v.byteOffset);
}
bool tablesMatch(const Tables& tables,const TextureResourceLifecycleView& view,uint64_t maxWork) noexcept {
  Check check{maxWork};
  if(!masksValid(view,check)) return false;
  for(size_t d=0;d<3;++d) for(size_t k=0;k<size_t(Kind::Count);++k) if(supported(Domain(d),Kind(k))) {
    const auto& rows=tables[table(Domain(d),Kind(k))];if(rows.size()!=count(view,Domain(d),Kind(k))) return false;
    for(size_t i=0;i<rows.size();++i) if(!payloadMatches(rows[i],view,Domain(d),Kind(k),i,check)) return false;
  }
  for(const auto domain:{Domain::Archive,Domain::Evaluated}) {
    const auto& textures=tables[table(domain,Kind::Texture)];
    const auto& images=tables[table(domain,Kind::Image)];const auto& samplers=tables[table(domain,Kind::Sampler)];
    for(const auto& row:textures) if(row.active) {
      if(!row.payload) return false;
      for(const auto index:row.payload->dependencies.images()) if(!check.take() || index>=images.size() || !images[index].active) return false;
      for(const auto index:row.payload->dependencies.samplers()) if(!check.take() || index>=samplers.size() || !samplers[index].active) return false;
    }
  }
  return true;
}
uint64_t tableBytes(const Tables& tables) {
  uint64_t bytes=0;for(const auto& rows:tables) for(const auto& row:rows) bytes=plus(bytes,plus(sizeof(Row),row.payload?row.payload->bytes:0));return bytes;
}
uint64_t generationBytes(const Tables& tables) {
  // Immutable resident accounting is independent of other catalog pin owners,
  // pending registrations, history branches and monotonic catalog high waters.
  uint64_t bytes=sizeof(TextureResourceLifecycleGeneration)+sizeof(Authority)+1024;
  for(const auto& rows:tables) {
    bytes=plus(bytes,times(rows.capacity(),sizeof(Row)+256));
    for(const auto& row:rows) if(row.payload) bytes=plus(bytes,row.payload->bytes);
  }
  return bytes;
}
}

struct TextureResourceReferenceOwner::State {
  std::vector<std::shared_ptr<Authority>> authorities;
  std::vector<SelectedRow> rows;
  std::vector<std::shared_ptr<PinControl>> pins;
  std::vector<std::shared_ptr<OpaqueBlocker>> blockers;
  std::vector<ResourceHandle> resources;
  uint64_t bytes{};
};
struct TextureResourceLiveOwner::State {
  std::shared_ptr<Authority> authority;
  TextureResourceLiveOwnerKey key;
  std::vector<ResourceHandle> targets;
  ResourcePin pin;
  std::shared_ptr<OpaqueBlocker> blocker;
  std::shared_ptr<const Payload> sourceWitness;
  std::weak_ptr<const TextureResourceLiveOwner> predecessor;
  uint64_t bytes{};
  bool pending{true},enabled{},cancelled{},counted{};
  ~State() {if(counted) {--authority->liveOwners;authority->liveReferences-=targets.size();}}
};
struct RoleSnapshot {
  bool known{};
  std::vector<uint32_t> color,data;
};
struct TextureResourceLifecycleGeneration::State {
  std::shared_ptr<Authority> authority;
  Tables tables;
  uint64_t revision{},bytes{},maxWork{};
  bool published{};
  std::array<RoleSnapshot,2> roles;
  std::vector<int32_t> archiveToEvaluated;
  std::vector<std::shared_ptr<const uint8_t>> archiveTextureIdentities;
  std::vector<TextureResourceLiveOwnerPtr> sourceOwners;
};
struct TextureResourceLifecycleCommand::State {
  std::shared_ptr<Authority> authority;
  Tables tails;
  std::array<Prefix,tableCount> prefix;
  TextureResourceReferenceOwnerPtr references;
  ResourceAppendCommand appendAuthority;
  uint64_t bytes{},version{};
  bool applied{},published{};
  std::array<RoleSnapshot,2> roleTails;
  std::vector<int32_t> mappingTail;
  std::vector<std::shared_ptr<const uint8_t>> identityTail;
};
struct RetirementTextureEndpoint {
  TextureResourceSelection selection;
  bool rolesKnown{},mappingKnown{},cookieKnown{},retireOwner{true},colorAffected{true},dataAffected{true};
  uint32_t color{UINT32_MAX},data{UINT32_MAX};
  int32_t mapping{-1};
  ResourceHandle colorHandle,dataHandle,mappedHandle;
  ResourceHandle sourceHandle;
  std::shared_ptr<const uint8_t> cookie;
};
struct TextureResourceRetirementCommand::State {
  std::shared_ptr<Authority> authority;
  std::vector<TextureResourceSelection> selections;
  std::vector<Row> rows;
  std::vector<RetirementTextureEndpoint> textures;
  TextureResourceReferenceOwnerPtr references;
  ResourceRetirementCommand retirement;
  uint64_t bytes{},version{};
  bool retired{},published{};
};
struct PreparedTextureResourceLifecycle::State {
  TextureResourceLifecycleGenerationPtr before,next;
  TextureResourceLifecycleCommand command;
  TextureResourceRetirementCommand retirementCommand;
  PreparedResourceRegistration registration;
  PreparedResourceTransition transition;
  ResourceRetirementCommand replayAuthority;
  ResourceAppendCommand appendReplayAuthority;
  std::vector<std::weak_ptr<OpaqueBlocker>> pendingBlockers;
  std::array<uint32_t,tableCount> expectedHigh{},nextHigh{};
  uint64_t revision{},commandVersion{},bytes{};
  uint64_t retirementVersion{};
  uint64_t expectedLiveVersion{},nextLiveVersion{};
  TextureResourceLifecycleLimits limits;
  std::vector<ResourceHandle> deferredRetirement;
  std::vector<TextureResourceLiveOwnerPtr> outgoing,incoming;
  std::map<LiveLabel,const TextureResourceLiveOwner*> incomingKeys;
  std::map<LiveLabel,const TextureResourceLiveOwner*> outgoingKeys;
  std::vector<LiveLabel> liveReservations;
  uint64_t liveWork{};
  bool replay{},forward{},consumed{},sealed{},failed{},retirementInitial{},retirementTarget{};
  ~State();
};

struct TextureResourceLifecycleAccess {
  using OwnerState=TextureResourceReferenceOwner::State;
  using GenerationState=TextureResourceLifecycleGeneration::State;
  using CommandState=TextureResourceLifecycleCommand::State;
  using RetirementState=TextureResourceRetirementCommand::State;
  using PreparedState=PreparedTextureResourceLifecycle::State;
  using LiveState=TextureResourceLiveOwner::State;
  static auto& get(const TextureResourceReferenceOwner& value) noexcept {return value.state_;}
  static auto& get(const TextureResourceLifecycleGeneration& value) noexcept {return value.state_;}
  static auto& get(const TextureResourceLifecycleCommand& value) noexcept {return value.state_;}
  static auto& get(const TextureResourceRetirementCommand& value) noexcept {return value.state_;}
  static auto& get(const PreparedTextureResourceLifecycle& value) noexcept {return value.state_;}
  static auto& get(const TextureResourceLiveOwner& value) noexcept {return value.state_;}
  static TextureResourceLiveOwnerPtr live(std::shared_ptr<LiveState> state) {
    auto result=std::make_shared<TextureResourceLiveOwner>();result->state_=std::move(state);return result;
  }
  static TextureResourceReferenceOwnerPtr owner(std::shared_ptr<OwnerState> state) {
    auto result=std::shared_ptr<TextureResourceReferenceOwner>(new TextureResourceReferenceOwner);result->state_=std::move(state);return result;
  }
  static TextureResourceLifecycleGenerationPtr generation(std::shared_ptr<GenerationState> state) {
    auto result=std::shared_ptr<TextureResourceLifecycleGeneration>(new TextureResourceLifecycleGeneration);result->state_=std::move(state);return result;
  }
  static TextureResourceLifecycleCommand command(std::shared_ptr<CommandState> state) {
    TextureResourceLifecycleCommand result;result.state_=std::move(state);return result;
  }
  static TextureResourceRetirementCommand retirement(std::shared_ptr<RetirementState> state) {
    TextureResourceRetirementCommand result;result.state_=std::move(state);return result;
  }
  static PreparedTextureResourceLifecycle prepared(std::unique_ptr<PreparedState> state) {
    PreparedTextureResourceLifecycle result;result.state_=std::move(state);return result;
  }
};

PreparedTextureResourceLifecycle::State::~State() {
  if(!consumed) {
    for(const auto& pending:pendingBlockers) if(auto blocker=pending.lock()) blocker->cancel();
    for(const auto& owner:incoming) if(owner) {
      auto& live=*TextureResourceLifecycleAccess::get(*owner);live.cancelled=true;
      if(live.blocker) live.blocker->cancel();
    }
  }
  if(next || before) {
    const auto& generation=next?TextureResourceLifecycleAccess::get(*next):TextureResourceLifecycleAccess::get(*before);
    auto& slots=generation->authority->liveSlots;
    for(const auto& key:liveReservations) {
      const auto found=slots.find(key);if(found==slots.end()) continue;
      if(!consumed && found->second.reservations) --found->second.reservations;
      const auto active=found->second.active.lock();
      if(!found->second.reservations && (!active || !active->active())) slots.erase(found);
    }
    for(const auto& owner:outgoing) {
      const auto found=slots.find(liveLabel(owner->key()));if(found==slots.end()) continue;
      const auto active=found->second.active.lock();
      if(!found->second.reservations && (!active || !active->active())) slots.erase(found);
    }
  }
}

namespace {
using Access=TextureResourceLifecycleAccess;
bool handleLess(const ResourceHandle& a,const ResourceHandle& b) noexcept {
  return std::tuple(a.domain().identity(),a.kind(),a.slot(),a.incarnation())<
      std::tuple(b.domain().identity(),b.kind(),b.slot(),b.incarnation());
}
bool selectionLess(const TextureResourceSelection& a,const TextureResourceSelection& b) noexcept {
  return std::tuple(a.domain,a.kind,a.row)<std::tuple(b.domain,b.kind,b.row);
}
bool ownerHasPending(const Access::OwnerState& owner,const ResourceHandle& handle) noexcept {
  for(const auto& pin:owner.pins) if(pin && pin->pin.pending())
    for(const auto& h:pin->pin.resources()) if(h==handle) return true;
  return false;
}
void finishOwner(Access::OwnerState& state,Budget& budget) {
  budget.charge(sizeof(TextureResourceReferenceOwner)+sizeof(Access::OwnerState)+128);
  budget.charge(times(state.rows.size(),sizeof(SelectedRow)));
  budget.charge(times(state.authorities.size(),sizeof(std::shared_ptr<Authority>)));
  budget.charge(times(state.pins.size(),sizeof(std::shared_ptr<PinControl>)+128));
  budget.charge(times(state.blockers.size(),sizeof(std::shared_ptr<OpaqueBlocker>)+128));
  for(const auto& row:state.rows) if(row.row.payload) budget.charge(row.row.payload->bytes);
  // Pin classes may span selected Material identities from another catalog.
  // Copy only those exact handles, never a material/resource parent generation.
  uint64_t references=state.rows.size();for(const auto& pin:state.pins) references=plus(references,pin->pin.resources().size());
  budget.charge(times(references,sizeof(ResourceHandle)));budget.visit(references);
  state.resources.reserve(references);
  for(const auto& row:state.rows) state.resources.push_back(row.row.handle);
  for(const auto& pin:state.pins) for(const auto& handle:pin->pin.resources()) state.resources.push_back(handle);
  budget.visit(times(state.resources.size(),plus(4,std::bit_width(state.resources.size()))));
  std::ranges::sort(state.resources,handleLess);
  // Equal numerical labels can belong to independent catalogs. Equality of
  // their opaque handles, rather than labels, decides deduplication.
  for(size_t begin=0;begin<state.resources.size();) {
    size_t end=begin+1;while(end<state.resources.size() && !handleLess(state.resources[begin],state.resources[end])) ++end;
    budget.visit(times(end-begin,end-begin));
    for(size_t i=begin;i<end;++i) for(size_t j=i+1;j<end;)
      if(state.resources[i]==state.resources[j]) {state.resources.erase(state.resources.begin()+j);--end;} else ++j;
    begin=end;
  }
  state.bytes=budget.bytes;
}
TextureResourceReferenceOwnerPtr selected(const std::shared_ptr<Access::GenerationState>& generation,
    std::span<const TextureResourceSelection> input,Pins kind,uint64_t maxBytes,bool opaque,Access::PreparedState* preparation=nullptr) {
  if(size_t(kind)>=size_t(Pins::Count)) invalid("Texture resource pin class is invalid");
  if(preparation && kind==Pins::LiveDependency) invalid("Future live edges require an explicit keyed live-owner control");
  TextureResourceLifecycleLimits limits;limits.maxOwnedBytes=maxBytes;Budget budget(limits);
  if(input.size()>limits.maxRows) exceeded();budget.charge(times(input.size(),sizeof(TextureResourceSelection)));
  budget.visit(times(input.size(),plus(4,std::bit_width(input.size()))));
  std::vector<TextureResourceSelection> selections(input.begin(),input.end());
  std::ranges::sort(selections,selectionLess);selections.erase(std::unique(selections.begin(),selections.end()),selections.end());
  const auto initial=selections.size();bool incomplete=opaque;
  for(size_t i=0;i<initial;++i) {
    const auto chosen=selections[i];budget.visit();
    if(!supported(chosen.domain,chosen.kind)) invalid("Texture resource selection kind/domain is invalid");
    const auto& rows=generation->tables[table(chosen.domain,chosen.kind)];
    if(chosen.row>=rows.size()) invalid("Texture resource selection row is missing");
    const auto& row=rows[chosen.row];
    if(!row.active && (kind==Pins::Preparation || kind==Pins::LiveDependency)) invalid("Inactive resource cannot own a future dependency/preparation");
    if(chosen.kind!=Kind::Texture) continue;
    const auto& refs=row.payload->dependencies;incomplete|=!refs.complete();
    for(const auto image:refs.images()) {budget.charge(sizeof(TextureResourceSelection));budget.visit();selections.push_back({chosen.domain,Kind::Image,image});}
    for(const auto sampler:refs.samplers()) {budget.charge(sizeof(TextureResourceSelection));budget.visit();selections.push_back({chosen.domain,Kind::Sampler,sampler});}
  }
  if(selections.size()>limits.maxRows) exceeded();
  budget.visit(times(selections.size(),plus(4,std::bit_width(selections.size()))));
  std::ranges::sort(selections,selectionLess);selections.erase(std::unique(selections.begin(),selections.end()),selections.end());
  budget.charge(sizeof(Access::OwnerState)+sizeof(TextureResourceReferenceOwner)+256);
  budget.charge(times(selections.size(),sizeof(SelectedRow)+sizeof(ResourceHandle)));
  for(const auto chosen:selections) {
    const auto& rows=generation->tables[table(chosen.domain,chosen.kind)];
    if(chosen.row>=rows.size()) invalid("Texture dependency row is missing");
    if(rows[chosen.row].payload) {
      budget.charge(rows[chosen.row].payload->bytes);incomplete|=rows[chosen.row].payload->opaque;
    }
  }
  auto result=std::make_shared<Access::OwnerState>();result->authorities.push_back(generation->authority);result->rows.reserve(selections.size());
  std::vector<ResourceHandle> handles;budget.charge(times(selections.size(),sizeof(ResourceHandle)));handles.reserve(selections.size());
  for(const auto chosen:selections) {
    const auto& rows=generation->tables[table(chosen.domain,chosen.kind)];
    if(chosen.row>=rows.size()) invalid("Texture dependency row is missing");
    const auto& row=rows[chosen.row];
    if(!row.active && (kind==Pins::Preparation || kind==Pins::LiveDependency)) invalid("Inactive texture dependency cannot be prepared");
    result->rows.push_back({generation->authority,row});handles.push_back(row.handle);
  }
  if(!handles.empty()) {
    ResourcePin pin;
    if(preparation) {
      if(preparation->registration) pin=generation->authority->catalog->stagePin(preparation->registration,handles,kind,"Future selected texture resource endpoint");
      else if(preparation->transition) pin=generation->authority->catalog->stagePin(preparation->transition,handles,kind,"Future selected texture resource endpoint");
      else pin=generation->authority->catalog->pin(handles,kind,"Surviving active candidate resource endpoint");
    } else pin=generation->authority->catalog->pin(handles,kind,"Selected texture resource endpoint");
    result->pins.push_back(std::make_shared<PinControl>(std::move(pin)));
  }
  if(incomplete) {
    auto blocker=std::make_shared<OpaqueBlocker>(generation->authority);
    if(preparation) {
      budget.visit(preparation->pendingBlockers.size()+1);
      std::erase_if(preparation->pendingBlockers,[](const auto& pending){return pending.expired();});
      if(preparation->pendingBlockers.size()>=generation->authority->maxOpaque) exceeded();
      preparation->pendingBlockers.push_back(blocker);
    }
    result->blockers.push_back(std::move(blocker));
  }
  finishOwner(*result,budget);return Access::owner(std::move(result));
}
std::vector<TextureResourceSelection> allSelections(const Access::GenerationState& generation,uint64_t maxBytes) {
  uint64_t n=0;for(const auto& rows:generation.tables) for(const auto& row:rows) if(row.active) ++n;
  if(n>1'000'000 || times(n,sizeof(TextureResourceSelection))>maxBytes) exceeded();
  std::vector<TextureResourceSelection> result;result.reserve(n);
  for(size_t d=0;d<3;++d) for(size_t k=0;k<size_t(Kind::Count);++k) if(supported(Domain(d),Kind(k))) {
    const auto& rows=generation.tables[table(Domain(d),Kind(k))];
    for(uint32_t i=0;i<rows.size();++i) if(rows[i].active) result.push_back({Domain(d),Kind(k),i});
  }
  return result;
}
void checkPrefix(const Access::CommandState& command,const Access::GenerationState& generation,bool applied) {
  if(command.authority!=generation.authority || !command.published || command.applied!=applied) invalid("Texture resource history belongs to another phase or catalog");
  for(size_t t=0;t<tableCount;++t) {
    const auto& rows=generation.tables[t];const auto& prefix=command.prefix[t];
    const size_t expected=prefix.count+(applied?command.tails[t].size():0);
    if(rows.size()!=expected || (prefix.count && rows[prefix.count-1].handle!=prefix.last)) invalid("Texture resource history prefix belongs to another branch");
    if(applied) for(size_t i=0;i<command.tails[t].size();++i)
      if(rows[prefix.count+i].handle!=command.tails[t][i].handle || rows[prefix.count+i].payload!=command.tails[t][i].payload ||
          rows[prefix.count+i].active!=command.tails[t][i].active)
        invalid("Texture resource history no longer owns its suffix");
  }
}
bool spanEqual(const auto& a,const auto& b) noexcept {return a.size()==b.size() && std::equal(a.begin(),a.end(),b.begin());}
bool roleViewMatches(const Access::GenerationState& generation,const TextureResourceLifecycleView& view) noexcept {
  for(size_t d=0;d<2;++d) {
    const auto& input=d?view.evaluated:view.archive;const auto& expected=generation.roles[d];
    if(input.rolesKnown!=expected.known || !spanEqual(input.colorResources,expected.color) || !spanEqual(input.dataResources,expected.data)) return false;
  }
  return spanEqual(view.archiveToEvaluated,generation.archiveToEvaluated) &&
      spanEqual(view.archiveTextureIdentities,generation.archiveTextureIdentities);
}
void captureRoles(Access::GenerationState& generation,const TextureResourceLifecycleView& view,Budget& budget) {
  for(size_t d=0;d<2;++d) {
    const auto& input=d?view.evaluated:view.archive;auto& output=generation.roles[d];
    if(input.rolesKnown) {
      if(input.colorResources.size()!=input.textures.size() || input.dataResources.size()!=input.textures.size()) invalid("Known source texture roles require exact complete tables");
    } else if(!input.colorResources.empty() || !input.dataResources.empty()) invalid("Unproved source roles must be explicitly marked known");
    budget.charge(times(plus(input.colorResources.size(),input.dataResources.size()),sizeof(uint32_t)));
    budget.visit(plus(input.colorResources.size(),input.dataResources.size()));
    for(const auto roles:{input.colorResources,input.dataResources}) for(size_t row=0;row<roles.size();++row) {
      const auto runtime=roles[row];
      if(!viewActive(view,Domain(d),Kind::Texture,row) && runtime!=UINT32_MAX)
        invalid("Inactive source texture must have cleared runtime roles");
      if(runtime!=UINT32_MAX && !view.runtime.isTextureActive(runtime)) invalid("Source texture role targets a missing or inactive runtime texture");
    }
    output.known=input.rolesKnown;output.color.assign(input.colorResources.begin(),input.colorResources.end());output.data.assign(input.dataResources.begin(),input.dataResources.end());
  }
  if((!view.archiveToEvaluated.empty() && view.archiveToEvaluated.size()!=view.archive.textures.size()) ||
      (!view.archiveTextureIdentities.empty() && view.archiveTextureIdentities.size()!=view.archive.textures.size()) ||
      (view.archive.rolesKnown && (view.archiveToEvaluated.size()!=view.archive.textures.size() || view.archiveTextureIdentities.size()!=view.archive.textures.size())))
    invalid("Source role correspondence requires exact archive mappings and identity cookies");
  budget.charge(times(view.archiveToEvaluated.size(),sizeof(int32_t)));budget.charge(times(view.archiveTextureIdentities.size(),sizeof(std::shared_ptr<const uint8_t>)));
  budget.visit(plus(view.archiveToEvaluated.size(),view.archiveTextureIdentities.size()));
  for(size_t row=0;row<view.archiveToEvaluated.size();++row) {
    const auto target=view.archiveToEvaluated[row];
    if(target< -1 || (target>=0 && (size_t(target)>=view.evaluated.textures.size() ||
        !viewActive(view,Domain::Evaluated,Kind::Texture,size_t(target)))) ||
        (!viewActive(view,Domain::Archive,Kind::Texture,row) && target!=-1))
      invalid("Archive role correspondence targets an inactive/missing texture or has an inactive owner");
  }
  for(const auto& cookie:view.archiveTextureIdentities) if(!cookie) invalid("Source texture identity cookie is absent");
  generation.archiveToEvaluated.assign(view.archiveToEvaluated.begin(),view.archiveToEvaluated.end());
  generation.archiveTextureIdentities.assign(view.archiveTextureIdentities.begin(),view.archiveTextureIdentities.end());
}
void captureRoleTails(Access::CommandState& command,const Access::GenerationState& next,Budget& budget) {
  for(size_t d=0;d<2;++d) {
    const auto begin=command.prefix[table(Domain(d),Kind::Texture)].count;
    const auto count=command.tails[table(Domain(d),Kind::Texture)].size();auto& output=command.roleTails[d];output.known=next.roles[d].known;
    if(count && output.known) {
      budget.charge(times(count,sizeof(uint32_t)*2));
      output.color.assign(next.roles[d].color.begin()+begin,next.roles[d].color.end());output.data.assign(next.roles[d].data.begin()+begin,next.roles[d].data.end());
    }
  }
  const auto begin=command.prefix[table(Domain::Archive,Kind::Texture)].count;
  const auto count=command.tails[table(Domain::Archive,Kind::Texture)].size();
  if(count && !next.archiveToEvaluated.empty()) {budget.charge(times(count,sizeof(int32_t)));command.mappingTail.assign(next.archiveToEvaluated.begin()+begin,next.archiveToEvaluated.end());}
  if(count && !next.archiveTextureIdentities.empty()) {budget.charge(times(count,sizeof(std::shared_ptr<const uint8_t>)));command.identityTail.assign(next.archiveTextureIdentities.begin()+begin,next.archiveTextureIdentities.end());}
}
bool roleTailsMatch(const Access::CommandState& command,const Access::GenerationState& generation) noexcept {
  for(size_t d=0;d<2;++d) {
    const auto begin=command.prefix[table(Domain(d),Kind::Texture)].count;
    const auto count=command.tails[table(Domain(d),Kind::Texture)].size();if(!count) continue;
    const auto& expected=command.roleTails[d];const auto& current=generation.roles[d];
    if(current.known!=expected.known) return false;
    if(current.known && (current.color.size()<begin+count || current.data.size()<begin+count ||
        !spanEqual(std::span(current.color).subspan(begin,count),expected.color) || !spanEqual(std::span(current.data).subspan(begin,count),expected.data))) return false;
  }
  const auto begin=command.prefix[table(Domain::Archive,Kind::Texture)].count;
  if(!command.mappingTail.empty() && (generation.archiveToEvaluated.size()<begin+command.mappingTail.size() ||
      !spanEqual(std::span(generation.archiveToEvaluated).subspan(begin,command.mappingTail.size()),command.mappingTail))) return false;
  if(!command.identityTail.empty() && (generation.archiveTextureIdentities.size()<begin+command.identityTail.size() ||
      !spanEqual(std::span(generation.archiveTextureIdentities).subspan(begin,command.identityTail.size()),command.identityTail))) return false;
  return true;
}
const RetirementTextureEndpoint* retirementEndpoint(const Access::RetirementState& command,Domain domain,uint32_t row) noexcept {
  const TextureResourceSelection selection{domain,Kind::Texture,row};
  const auto found=std::lower_bound(command.textures.begin(),command.textures.end(),selection,
      [](const auto& endpoint,const auto chosen) {return selectionLess(endpoint.selection,chosen);});
  return found!=command.textures.end() && found->selection==selection?&*found:nullptr;
}
bool retirementEndpointMatches(const Access::RetirementState& command,const Access::GenerationState& generation,bool active) noexcept {
  for(size_t i=0;i<command.selections.size();++i) {
    const auto selection=command.selections[i];const auto& rows=generation.tables[table(selection.domain,selection.kind)];
    if(selection.row>=rows.size() || rows[selection.row].handle!=command.rows[i].handle ||
        rows[selection.row].payload!=command.rows[i].payload || rows[selection.row].active!=active) return false;
  }
  for(const auto& endpoint:command.textures) {
    const auto domain=endpoint.selection.domain;const auto row=endpoint.selection.row;const auto& roles=generation.roles[size_t(domain)];
    const auto& sources=generation.tables[table(domain,Kind::Texture)];
    if(row>=sources.size() || sources[row].handle!=endpoint.sourceHandle || sources[row].active!=(endpoint.retireOwner?active:true)) return false;
    if(roles.known!=endpoint.rolesKnown) return false;
    if(roles.known && (row>=roles.color.size() || row>=roles.data.size() ||
        (endpoint.colorAffected && roles.color[row]!=(active?endpoint.color:UINT32_MAX)) ||
        (endpoint.dataAffected && roles.data[row]!=(active?endpoint.data:UINT32_MAX)))) return false;
    if(active && roles.known) {
      const auto& runtime=generation.tables[table(Domain::Runtime,Kind::RuntimeTexture)];
      for(const auto& [affected,index,handle]:{std::tuple{endpoint.colorAffected,endpoint.color,endpoint.colorHandle},
          std::tuple{endpoint.dataAffected,endpoint.data,endpoint.dataHandle}})
        if(affected && index!=UINT32_MAX && (index>=runtime.size() || !runtime[index].active || runtime[index].handle!=handle)) return false;
    }
    if(domain==Domain::Archive) {
      if((endpoint.retireOwner && (endpoint.mappingKnown!=(generation.archiveToEvaluated.size()>row) ||
          (endpoint.mappingKnown && generation.archiveToEvaluated[row]!=(active?endpoint.mapping:-1)))) ||
          endpoint.cookieKnown!=(generation.archiveTextureIdentities.size()>row) ||
          (endpoint.cookieKnown && generation.archiveTextureIdentities[row]!=endpoint.cookie)) return false;
      if(active && endpoint.retireOwner && endpoint.mapping>=0) {
        const auto& evaluated=generation.tables[table(Domain::Evaluated,Kind::Texture)];
        if(size_t(endpoint.mapping)>=evaluated.size() || !evaluated[size_t(endpoint.mapping)].active ||
            evaluated[size_t(endpoint.mapping)].handle!=endpoint.mappedHandle) return false;
      }
    }
  }
  return true;
}
bool retirementRolesChangeOnlySelected(const Access::RetirementState& command,const Access::GenerationState& before,
    const Access::GenerationState& next,bool active) noexcept {
  if(!retirementEndpointMatches(command,next,active)) return false;
  for(size_t d=0;d<2;++d) {
    const auto& old=before.roles[d];const auto& current=next.roles[d];
    if(old.known!=current.known || old.color.size()!=current.color.size() || old.data.size()!=current.data.size()) return false;
    for(size_t i=0;i<old.color.size();++i) {
      const auto endpoint=retirementEndpoint(command,Domain(d),uint32_t(i));
      if(((!endpoint || !endpoint->colorAffected) && old.color[i]!=current.color[i]) ||
          ((!endpoint || !endpoint->dataAffected) && old.data[i]!=current.data[i])) return false;
    }
  }
  if(before.archiveToEvaluated.size()!=next.archiveToEvaluated.size() ||
      !spanEqual(before.archiveTextureIdentities,next.archiveTextureIdentities)) return false;
  for(size_t i=0;i<before.archiveToEvaluated.size();++i) {
    const auto endpoint=retirementEndpoint(command,Domain::Archive,uint32_t(i));
    if((!endpoint || !endpoint->retireOwner) && before.archiveToEvaluated[i]!=next.archiveToEvaluated[i]) return false;
  }
  return true;
}
void prepareAutomaticLiveOwners(PreparedTextureResourceLifecycle&);
}

TextureResourceLiveOwnerKey TextureResourceLiveOwner::key() const noexcept {return state_?state_->key:TextureResourceLiveOwnerKey{};}
bool TextureResourceLiveOwner::valid() const noexcept {
  return state_ && !state_->cancelled && state_->authority && state_->key.owner &&
      (!state_->blocker || state_->blocker->valid) && (state_->pending || state_->enabled);
}
bool TextureResourceLiveOwner::active() const noexcept {
  return valid() && !state_->pending && state_->enabled &&
      (state_->targets.empty() || (state_->pin.active() && state_->pin.resourcesActive()));
}
bool TextureResourceLiveOwner::pending() const noexcept {return valid() && state_->pending;}
bool TextureResourceLiveOwner::matches(const TextureResourceLifecycleGenerationPtr& generation) const noexcept {
  if(!valid() || !generation || Access::get(*generation)->authority!=state_->authority) return false;
  const auto& next=*Access::get(*generation);
  for(const auto& handle:state_->targets) {
    if(!supported(handle.domain().kind(),handle.kind())) return false;
    const auto& rows=next.tables[table(handle.domain().kind(),handle.kind())];
    const auto found=std::lower_bound(rows.begin(),rows.end(),handle.slot(),[](const auto& row,uint32_t slot){return row.handle.slot()<slot;});
    if(found==rows.end() || found->handle!=handle || !found->active) return false;
  }
  return true;
}
bool TextureResourceLiveOwner::complete() const noexcept {return valid() && !state_->blocker;}
uint64_t TextureResourceLiveOwner::ownedBytes() const noexcept {return state_?state_->bytes:0;}
std::span<const ResourceHandle> TextureResourceLiveOwner::resources() const noexcept {return state_?std::span<const ResourceHandle>(state_->targets):std::span<const ResourceHandle>{};}

uint64_t TextureResourceReferenceOwner::ownedBytes() const noexcept {return state_?state_->bytes:0;}
bool TextureResourceReferenceOwner::valid() const noexcept {
  if(!state_) return false;
  for(const auto& pin:state_->pins) if(!pin || !pin->pin) return false;
  for(const auto& blocker:state_->blockers) if(!blocker || !blocker->valid) return false;
  for(const auto& row:state_->rows)
    if(!row.authority || (!row.authority->catalog->contains(row.row.handle) && !ownerHasPending(*state_,row.row.handle))) return false;
  return true;
}
bool TextureResourceReferenceOwner::active() const noexcept {
  if(!resourceDependenciesActive()) return false;
  for(const auto& pin:state_->pins) if(!pin->pin.resourcesActive()) return false;
  return true;
}
bool TextureResourceReferenceOwner::resourceDependenciesActive() const noexcept {
  if(!valid()) return false;
  for(const auto& row:state_->rows) if(!row.authority->catalog->active(row.row.handle)) return false;
  return true;
}
bool TextureResourceReferenceOwner::resourceDependenciesMatch(const TextureResourceLifecycleGenerationPtr& generation) const noexcept {
  if(!valid() || !generation || !generation->valid()) return false;
  const auto& current=*Access::get(*generation);
  for(const auto& authority:state_->authorities) if(authority!=current.authority) return false;
  for(const auto& selected:state_->rows) {
    if(selected.authority!=current.authority) return false;
    const auto& handle=selected.row.handle;
    if(!supported(handle.domain().kind(),handle.kind())) return false;
    const auto& rows=current.tables[table(handle.domain().kind(),handle.kind())];
    const auto found=std::lower_bound(rows.begin(),rows.end(),handle.slot(),[](const auto& row,uint32_t slot){return row.handle.slot()<slot;});
    if(found==rows.end() || found->handle!=handle) return false;
  }
  return true;
}
bool TextureResourceReferenceOwner::complete() const noexcept {return state_ && state_->blockers.empty();}
std::span<const ResourceHandle> TextureResourceReferenceOwner::resources() const noexcept {return state_?std::span<const ResourceHandle>(state_->resources):std::span<const ResourceHandle>{};}
uint64_t TextureResourceLifecycleGeneration::ownedBytes() const noexcept {return state_?state_->bytes:0;}
uint64_t TextureResourceLifecycleGeneration::revision() const noexcept {return state_?state_->revision:0;}
bool TextureResourceLifecycleGeneration::sharesAuthority(const TextureResourceLifecycleGenerationPtr& other) const noexcept {
  return state_ && other && other->state_ && state_->authority==other->state_->authority;
}
uint64_t TextureResourceLifecycleGeneration::opaqueReferenceOwnerCount() const noexcept {return state_?state_->authority->opaque:0;}
bool TextureResourceLifecycleGeneration::valid() const noexcept {
  if(!state_ || !state_->published) return false;
  for(const auto& rows:state_->tables) for(const auto& row:rows)
    if(!state_->authority->catalog->contains(row.handle) || state_->authority->catalog->active(row.handle)!=row.active) return false;
  return true;
}
bool TextureResourceLifecycleGeneration::matches(const TextureResourceLifecycleView& view) const noexcept {
  return state_ && valid() && liveDependenciesValid() && roleViewMatches(*state_,view) && tablesMatch(state_->tables,view,state_->maxWork);
}
bool TextureResourceLifecycleGeneration::liveDependenciesValid() const noexcept {
  if(!state_ || !state_->published) return false;
  for(const auto& owner:state_->sourceOwners) if(!owner || !owner->active()) return false;return true;
}
std::span<const TextureResourceLiveOwnerPtr> TextureResourceLifecycleGeneration::sourceLiveOwners() const noexcept {
  return state_?std::span<const TextureResourceLiveOwnerPtr>(state_->sourceOwners):std::span<const TextureResourceLiveOwnerPtr>{};
}
ResourceHandle TextureResourceLifecycleGeneration::resource(TextureResourceSelection chosen) const noexcept {
  if(!state_ || !supported(chosen.domain,chosen.kind)) return {};
  const auto& rows=state_->tables[table(chosen.domain,chosen.kind)];return chosen.row<rows.size()?rows[chosen.row].handle:ResourceHandle{};
}
bool TextureResourceLifecycleGeneration::active(TextureResourceSelection chosen) const noexcept {
  if(!state_ || !supported(chosen.domain,chosen.kind)) return false;
  const auto& rows=state_->tables[table(chosen.domain,chosen.kind)];
  return chosen.row<rows.size() && rows[chosen.row].active &&
      (!state_->published || state_->authority->catalog->active(rows[chosen.row].handle));
}
size_t TextureResourceLifecycleGeneration::rowCount(Domain domain,Kind kind) const noexcept {
  return state_ && supported(domain,kind)?state_->tables[table(domain,kind)].size():0;
}
ResourceCatalogSnapshot TextureResourceLifecycleGeneration::inventory() const {return state_?state_->authority->catalog->inventory():ResourceCatalogSnapshot{};}
TextureResourceLifecycleCommand::operator bool() const noexcept {return bool(state_);}
uint64_t TextureResourceLifecycleCommand::ownedBytes() const noexcept {return state_?state_->bytes:0;}
TextureResourceReferenceOwnerPtr TextureResourceLifecycleCommand::references() const noexcept {return state_?state_->references:TextureResourceReferenceOwnerPtr{};}
TextureResourceRetirementCommand::operator bool() const noexcept {return bool(state_);}
uint64_t TextureResourceRetirementCommand::ownedBytes() const noexcept {return state_?state_->bytes:0;}
TextureResourceReferenceOwnerPtr TextureResourceRetirementCommand::references() const noexcept {return state_?state_->references:TextureResourceReferenceOwnerPtr{};}
std::span<const TextureResourceSelection> TextureResourceRetirementCommand::selections() const noexcept {
  return state_?std::span<const TextureResourceSelection>(state_->selections):std::span<const TextureResourceSelection>{};
}
PreparedTextureResourceLifecycle::PreparedTextureResourceLifecycle() noexcept=default;
PreparedTextureResourceLifecycle::~PreparedTextureResourceLifecycle()=default;
PreparedTextureResourceLifecycle::PreparedTextureResourceLifecycle(PreparedTextureResourceLifecycle&&) noexcept=default;
PreparedTextureResourceLifecycle& PreparedTextureResourceLifecycle::operator=(PreparedTextureResourceLifecycle&&) noexcept=default;
PreparedTextureResourceLifecycle::operator bool() const noexcept {return state_ && !state_->consumed && !state_->failed;}
uint64_t PreparedTextureResourceLifecycle::ownedBytes() const noexcept {return state_?state_->bytes:0;}
const TextureResourceLifecycleGenerationPtr& PreparedTextureResourceLifecycle::next() const noexcept {
  static const TextureResourceLifecycleGenerationPtr empty;return state_?state_->next:empty;
}
TextureResourceLifecycleCommand PreparedTextureResourceLifecycle::command() const noexcept {return state_?state_->command:TextureResourceLifecycleCommand{};}
TextureResourceRetirementCommand PreparedTextureResourceLifecycle::retirementCommand() const noexcept {return state_?state_->retirementCommand:TextureResourceRetirementCommand{};}
bool PreparedTextureResourceLifecycle::sealed() const noexcept {return state_ && state_->sealed && !state_->failed;}
bool PreparedTextureResourceLifecycle::liveOwnerWillBeActive(const TextureResourceLiveOwnerPtr& owner) const noexcept {
  if(!state_ || state_->consumed || state_->failed || !state_->next || !owner || !owner->valid() || !owner->matches(state_->next)) return false;
  const auto& generation=*Access::get(*state_->next);const auto& live=*Access::get(*owner);const auto& authority=*generation.authority;
  if(live.authority!=generation.authority || authority.catalog->revision()!=state_->revision || authority.high!=state_->expectedHigh ||
      authority.liveVersion!=state_->expectedLiveVersion) return false;
  const auto label=liveLabel(owner->key());const auto slot=authority.liveSlots.find(label);if(slot==authority.liveSlots.end()) return false;
  if(owner->pending()) {
    const auto incoming=state_->incomingKeys.find(label);
    if(incoming==state_->incomingKeys.end() || incoming->second!=owner.get() || !slot->second.reservations) return false;
    const auto active=slot->second.active.lock();return !active || !active->active() || active==live.predecessor.lock();
  }
  if(!owner->active() || slot->second.active.lock()!=owner) return false;
  const auto outgoing=state_->outgoingKeys.find(label);return outgoing==state_->outgoingKeys.end() || outgoing->second!=owner.get();
}
void PreparedTextureResourceLifecycle::releaseLiveOwners(std::span<const TextureResourceLiveOwnerPtr> owners) {
  if(!*this || state_->sealed || state_->failed) invalid("Cannot modify a sealed or consumed resource mutation");
  const auto& authority=Access::get(*state_->next)->authority;
  for(const auto& owner:owners) {
    if(!owner || !owner->active() || Access::get(*owner)->authority!=authority) invalid("Outgoing resource owner is stale or belongs to another catalog");
    const auto slot=authority->liveSlots.find(liveLabel(owner->key()));
    if(slot==authority->liveSlots.end() || slot->second.active.lock()!=owner) invalid("Outgoing resource owner is not the current exact owner version");
    const auto label=liveLabel(owner->key());
    state_->liveWork=plus(state_->liveWork,plus(1,std::bit_width(state_->outgoing.size()+1)));if(state_->liveWork>state_->limits.maxWork) exceeded();
    const auto existing=state_->outgoingKeys.find(label);
    if(existing!=state_->outgoingKeys.end()) {if(existing->second!=owner.get()) invalid("Outgoing owner label does not match its exact control");continue;}
    const auto added=plus(state_->bytes,plus(sizeof(TextureResourceLiveOwnerPtr)+512,times(owner->resources().size(),sizeof(ResourceHandle)*2)));if(added>state_->limits.maxOwnedBytes) exceeded();
    if(state_->outgoing.size()==state_->outgoing.capacity()) state_->outgoing.reserve(std::max<size_t>(4,state_->outgoing.capacity()*2));
    state_->outgoingKeys.emplace(label,owner.get());state_->outgoing.push_back(owner);state_->bytes=added;
  }
}
TextureResourceLiveOwnerPtr PreparedTextureResourceLifecycle::stageLiveOwner(TextureResourceLiveOwnerKey key,
    std::span<const TextureResourceSelection> selections,const TextureResourceLiveOwnerPtr& outgoing,uint64_t maxBytes,bool opaque) {
  if(!*this || state_->sealed || state_->failed || !key.owner || size_t(key.kind)>size_t(TextureResourceLiveOwnerKind::Provider)) invalid("Invalid live dependency candidate or owner key");
  if(outgoing && (!outgoing->active() || outgoing->key().owner!=key.owner || outgoing->key().kind!=key.kind)) invalid("Live dependency replacement has a different exact owner");
  if(outgoing && Access::get(*outgoing)->authority!=Access::get(*state_->next)->authority) invalid("Live dependency replacement belongs to another source catalog");
  const auto label=liveLabel(key);
  if(state_->incomingKeys.contains(label)) invalid("A resource mutation cannot stage two successors for one live owner");
  auto generation=Access::get(*state_->next);TextureResourceLifecycleLimits limits=state_->limits;
  const auto existing=generation->authority->liveSlots.find(label);
  if(existing!=generation->authority->liveSlots.end()) {
    const auto activeOwner=existing->second.active.lock();
    if(activeOwner && activeOwner->active() && activeOwner!=outgoing) invalid("Replacing an existing live owner requires its exact outgoing control");
  }
  limits.maxOwnedBytes=std::min(maxBytes,state_->limits.maxOwnedBytes-state_->bytes);Budget budget(limits);
  budget.charge(sizeof(TextureResourceLiveOwner)+sizeof(Access::LiveState)+384);
  budget.charge(times(selections.size(),sizeof(ResourceHandle)+sizeof(TextureResourceSelection)));budget.visit(selections.size());
  auto live=std::make_shared<Access::LiveState>();live->authority=generation->authority;live->key=key;live->predecessor=outgoing;live->targets.reserve(selections.size());
  for(const auto chosen:selections) {
    if(!supported(chosen.domain,chosen.kind)) invalid("Live dependency target kind/domain is unsupported");
    const auto& rows=generation->tables[table(chosen.domain,chosen.kind)];
    if(chosen.row>=rows.size() || !rows[chosen.row].active) invalid("Live dependency targets a missing or future inactive identity");
    if(size_t(key.kind)<=size_t(TextureResourceLiveOwnerKind::DataRole) && rows[chosen.row].handle==key.owner)
      invalid("A source resource/role live owner cannot add a self-dependency");
    live->targets.push_back(rows[chosen.row].handle);
  }
  budget.visit(times(live->targets.size(),plus(4,std::bit_width(live->targets.size()))));
  std::ranges::sort(live->targets,handleLess);live->targets.erase(std::unique(live->targets.begin(),live->targets.end()),live->targets.end());
  const auto ownerLimit=std::min(generation->authority->maxLiveOwners,state_->limits.maxLiveOwners);
  const auto referenceLimit=std::min(generation->authority->maxLiveReferences,state_->limits.maxLiveReferences);
  if(generation->authority->liveOwners>=ownerLimit || generation->authority->liveReferences>referenceLimit ||
      live->targets.size()>referenceLimit-generation->authority->liveReferences) exceeded();
  if(state_->nextLiveVersion==UINT64_MAX) exceeded();
  if(!key.version) live->key.version=state_->nextLiveVersion+1;
  else if(key.version<=state_->nextLiveVersion) invalid("Live dependency version must advance the prepared owner cursor");
  if(opaque) {budget.charge(sizeof(OpaqueBlocker)+128);live->blocker=std::make_shared<OpaqueBlocker>(generation->authority);}
  budget.charge(512+times(live->targets.size(),sizeof(ResourceHandle))); // Generic pending pin/control reservation.
  live->bytes=budget.bytes;auto result=Access::live(std::move(live));
  state_->liveWork=plus(state_->liveWork,plus(2,std::bit_width(state_->incoming.size()+state_->outgoing.size()+1)));if(state_->liveWork>state_->limits.maxWork) exceeded();
  const bool addOutgoing=outgoing && !state_->outgoingKeys.contains(label);
  const auto outgoingBytes=addOutgoing?plus(sizeof(TextureResourceLiveOwnerPtr)+512,times(outgoing->resources().size(),sizeof(ResourceHandle)*2)):0;
  const auto total=plus(state_->bytes,plus(result->ownedBytes(),outgoingBytes));if(total>state_->limits.maxOwnedBytes) exceeded();
  if(state_->incoming.size()==state_->incoming.capacity()) state_->incoming.reserve(std::max<size_t>(4,state_->incoming.capacity()*2));
  if(addOutgoing && state_->outgoing.size()==state_->outgoing.capacity()) state_->outgoing.reserve(std::max<size_t>(4,state_->outgoing.capacity()*2));
  if(state_->liveReservations.size()==state_->liveReservations.capacity()) state_->liveReservations.reserve(std::max<size_t>(4,state_->liveReservations.capacity()*2));
  if(addOutgoing) state_->outgoingKeys.emplace(label,outgoing.get());
  std::map<LiveLabel,LiveSlot>::iterator slot;
  try {state_->incomingKeys.emplace(label,result.get());slot=generation->authority->liveSlots.try_emplace(label).first;}
  catch(...) {state_->incomingKeys.erase(label);if(addOutgoing) state_->outgoingKeys.erase(label);throw;}
  ++slot->second.reservations;state_->liveReservations.push_back(label); // No fallible operation follows attachment.
  auto& attached=*Access::get(*result);attached.counted=true;++generation->authority->liveOwners;generation->authority->liveReferences+=attached.targets.size();
  if(addOutgoing) state_->outgoing.push_back(outgoing);
  state_->incoming.push_back(result);state_->bytes=total;state_->nextLiveVersion=result->key().version;return result;
}
void PreparedTextureResourceLifecycle::seal() {
  if(!*this || state_->failed) invalid("Cannot seal a consumed or failed resource mutation");if(state_->sealed) return;
  auto& state=*state_;auto& generation=*Access::get(*state.next);auto& catalog=*generation.authority->catalog;
  try {
    std::vector<const ResourcePin*> outgoing;outgoing.reserve(state.outgoing.size());
    for(const auto& owner:state.outgoing) {
      if(!owner->active()) invalid("Outgoing resource dependency became stale before sealing");
      const auto& live=*Access::get(*owner);if(!live.targets.empty()) outgoing.push_back(&live.pin);
    }
    if(!state.deferredRetirement.empty()) {
      if(state.appendReplayAuthority) state.transition=catalog.prepareAppendChange(state.appendReplayAuthority,ResourceLifecyclePhase::Retired,outgoing,state.limits.retirementPolicy);
      else if(state.replayAuthority) state.transition=catalog.prepareChange(state.replayAuthority,ResourceLifecyclePhase::Retired,outgoing,state.limits.retirementPolicy);
      else state.transition=catalog.prepareRetirement(state.deferredRetirement,outgoing,"Publish exact owned source resource retirement",state.limits.retirementPolicy);
      if(state.retirementCommand) state.replayAuthority=state.transition.command();
    } else if(state.registration) catalog.stageDependencyRelease(state.registration,outgoing);
    else catalog.stageDependencyRelease(state.transition,outgoing);
    for(const auto& owner:state.incoming) {
      auto& live=*Access::get(*owner);
      if(!live.targets.empty()) live.pin=state.registration?
          catalog.stagePin(state.registration,live.targets,Pins::LiveDependency,"Future exact resource live owner"):
          catalog.stagePin(state.transition,live.targets,Pins::LiveDependency,"Future exact resource live owner");
    }
    state.sealed=true;
  } catch(...) {
    state.failed=true;
    for(const auto& owner:state.incoming) {auto& live=*Access::get(*owner);live.cancelled=true;if(live.blocker) live.blocker->cancel();}
    for(const auto& pending:state.pendingBlockers) if(auto blocker=pending.lock()) blocker->cancel();
    throw;
  }
}
TextureResourceReferenceOwnerPtr PreparedTextureResourceLifecycle::select(std::span<const TextureResourceSelection> input,Pins kind,uint64_t maxBytes,bool opaque) const {
  if(!*this) invalid("Texture resource preparation is consumed or empty");
  return selected(Access::get(*state_->next),input,kind,maxBytes,opaque,state_.get());
}
TextureResourceReferenceOwnerPtr PreparedTextureResourceLifecycle::selectAll(Pins kind,uint64_t maxBytes) const {
  if(!*this) invalid("Texture resource preparation is consumed or empty");
  const auto selections=allSelections(*Access::get(*state_->next),maxBytes);
  return select(selections,kind,maxBytes);
}

namespace {
void prepareAutomaticLiveOwners(PreparedTextureResourceLifecycle& preparation) {
  auto& state=*Access::get(preparation);auto next=Access::get(*state.next);
  const auto old=state.before?Access::get(*state.before):std::shared_ptr<Access::GenerationState>{};
  Budget work(state.limits);
  const auto scratch=[&](uint64_t bytes) {
    const auto total=plus(state.bytes,bytes);if(total>state.limits.maxOwnedBytes) exceeded();work.charge(bytes);state.bytes=total;
  };
  std::map<LiveLabel,TextureResourceLiveOwnerPtr> previousOwners;std::set<LiveLabel> reused;
  if(old) for(const auto& owner:old->sourceOwners) {
    scratch(sizeof(LiveLabel)+sizeof(TextureResourceLiveOwnerPtr)+128);work.visit(plus(1,std::bit_width(previousOwners.size()+1)));
    previousOwners.emplace(liveLabel(owner->key()),owner);
  }
  uint64_t count=0;for(const auto d:{Domain::Archive,Domain::Evaluated}) {
    count=plus(count,times(next->tables[table(d,Kind::Texture)].size(),3));
    count=plus(count,plus(next->tables[table(d,Kind::Image)].size(),next->tables[table(d,Kind::Sampler)].size()));
  }
  const auto vectorBytes=times(count,sizeof(TextureResourceLiveOwnerPtr));
  if(plus(state.bytes,vectorBytes)>state.limits.maxOwnedBytes) exceeded();state.bytes=plus(state.bytes,vectorBytes);next->sourceOwners.reserve(count);
  const auto addOwner=[&](Domain domain,Kind ownerKind,uint32_t index,TextureResourceLiveOwnerKind kind,
      std::span<const TextureResourceSelection> selected,bool opaque) {
    const auto& row=next->tables[table(domain,ownerKind)][index];if(!row.active) return;
    TextureResourceLiveOwnerPtr previous;const auto label=liveLabel({row.handle,kind,0});
    work.visit(plus(1,std::bit_width(previousOwners.size()+1)));
    if(const auto found=previousOwners.find(label);found!=previousOwners.end()) previous=found->second;
    bool same=previous && previous->active() && Access::get(*previous)->sourceWitness==row.payload && previous->complete()==!opaque && previous->resources().size()==selected.size();
    if(same) for(size_t i=0;i<selected.size();++i) {
      work.visit();const auto target=selected[i];
      if(previous->resources()[i]!=next->tables[table(target.domain,target.kind)][target.row].handle) {same=false;break;}
    }
    if(same) {
      if(plus(state.bytes,previous->ownedBytes())>state.limits.maxOwnedBytes) exceeded();state.bytes=plus(state.bytes,previous->ownedBytes());
      scratch(sizeof(LiveLabel)+128);work.visit(plus(1,std::bit_width(reused.size()+1)));reused.insert(label);
      next->sourceOwners.push_back(previous);return;
    }
    auto owner=preparation.stageLiveOwner({row.handle,kind,0},selected,previous,state.limits.maxOwnedBytes-state.bytes,opaque);
    Access::get(*owner)->sourceWitness=row.payload;next->sourceOwners.push_back(std::move(owner));
  };
  for(const auto domain:{Domain::Archive,Domain::Evaluated}) {
    for(const auto kind:{Kind::Image,Kind::Sampler}) {
      const auto& rows=next->tables[table(domain,kind)];
      for(uint32_t i=0;i<rows.size();++i) {work.visit();if(rows[i].active && rows[i].payload && rows[i].payload->opaque)
        addOwner(domain,kind,i,TextureResourceLiveOwnerKind::SourceRecord,{},true);}
    }
    const auto& rows=next->tables[table(domain,Kind::Texture)];const auto& roles=next->roles[size_t(domain)];
    for(uint32_t i=0;i<rows.size();++i) {
      work.visit();const auto& row=rows[i];if(!row.active) continue;
      const auto& refs=row.payload->dependencies;std::vector<TextureResourceSelection> selected;
      const auto n=plus(refs.images().size(),refs.samplers().size());scratch(times(n,sizeof(TextureResourceSelection)));work.visit(n);selected.reserve(n);
      for(const auto image:refs.images()) selected.push_back({domain,Kind::Image,image});
      for(const auto sampler:refs.samplers()) selected.push_back({domain,Kind::Sampler,sampler});
      addOwner(domain,Kind::Texture,i,TextureResourceLiveOwnerKind::SourceTexture,selected,row.payload->opaque || !refs.complete());
      if(roles.known) for(const bool color:{true,false}) {
        const auto target=(color?roles.color:roles.data)[i];if(target==UINT32_MAX) continue;
        const TextureResourceSelection selection{Domain::Runtime,Kind::RuntimeTexture,target};
        addOwner(domain,Kind::Texture,i,color?TextureResourceLiveOwnerKind::ColorRole:TextureResourceLiveOwnerKind::DataRole,
            std::span(&selection,1),false);
      }
    }
  }
  if(old) for(const auto& previous:old->sourceOwners) {
    work.visit(plus(1,std::bit_width(reused.size()+1)));
    if(!reused.contains(liveLabel(previous->key()))) preparation.releaseLiveOwners(std::span(&previous,1));
  }
  uint64_t resident=times(next->sourceOwners.capacity(),sizeof(TextureResourceLiveOwnerPtr));
  for(const auto& owner:next->sourceOwners) resident=plus(resident,owner->ownedBytes());
  for(const auto& roles:next->roles) resident=plus(resident,times(plus(roles.color.capacity(),roles.data.capacity()),sizeof(uint32_t)));
  resident=plus(resident,times(next->archiveToEvaluated.capacity(),sizeof(int32_t)));
  resident=plus(resident,times(next->archiveTextureIdentities.capacity(),sizeof(std::shared_ptr<const uint8_t>)));
  next->bytes=plus(next->bytes,resident);
}
}

static PreparedTextureResourceLifecycle prepareTextureResourceLifecycle(const TextureResourceLifecycleGenerationPtr& before,
    const TextureResourceLifecycleView& view,const TextureResourceLifecycleLimits& limits,bool reconcile) {
  Budget budget(limits);budget.charge(sizeof(Access::PreparedState)+sizeof(Access::GenerationState)+256);
  Check maskCheck{limits.maxWork};if(!masksValid(view,maskCheck)) invalid("Source resource phases require exact zero/one masks");
  budget.visit(limits.maxWork-maskCheck.remaining);
  auto state=std::make_unique<Access::PreparedState>();state->before=before;state->limits=limits;
  auto next=std::make_shared<Access::GenerationState>();next->maxWork=limits.maxWork;
  if(before) {
    if(!before->valid() || !before->liveDependenciesValid()) invalid("Current texture resource generation has stale phases or live controls");
    next->authority=Access::get(*before)->authority;
  } else {
    budget.charge(sizeof(Authority)+sizeof(ResourceLifecycleCatalog)+256);
    next->authority=std::make_shared<Authority>();next->authority->maxOpaque=limits.maxRows;
    next->authority->maxLiveOwners=limits.maxLiveOwners;next->authority->maxLiveReferences=limits.maxLiveReferences;
    ResourceLifecycleLimits catalogLimits;catalogLimits.maxDomains=3;catalogLimits.maxSlots=limits.maxRows;
    catalogLimits.maxPinSets=plus(limits.maxLiveOwners,4096);catalogLimits.maxPinnedReferences=limits.maxLiveReferences;
    catalogLimits.maxOwnedBytes=limits.maxOwnedBytes;catalogLimits.maxWork=limits.maxWork;
    next->authority->catalog=std::make_shared<ResourceLifecycleCatalog>(catalogLimits);
    for(size_t d=0;d<3;++d) next->authority->domains[d]=next->authority->catalog->createDomain(Domain(d));
  }
  auto& authority=*next->authority;state->expectedHigh=authority.high;state->nextHigh=authority.high;
  state->expectedLiveVersion=state->nextLiveVersion=authority.liveVersion;
  uint64_t total=0,newRows=0;std::vector<ResourceHandle> deactivate;
  Check check{limits.maxWork};
  for(size_t d=0;d<3;++d) for(size_t k=0;k<size_t(Kind::Count);++k) if(supported(Domain(d),Kind(k))) {
    const auto t=table(Domain(d),Kind(k));const auto n=count(view,Domain(d),Kind(k));
    total=plus(total,n);if(total>limits.maxRows) exceeded();
    const auto* old=before?&Access::get(*before)->tables[t]:nullptr;
    const auto oldCount=old?old->size():0;
    if(n<oldCount && (!reconcile || Domain(d)==Domain::Runtime))
      invalid("Texture resource shrink requires its original history command");
    budget.charge(times(n,sizeof(Row)));budget.visit(n);next->tables[t].reserve(n);
    const auto common=std::min(n,oldCount);
    for(size_t i=0;i<common;++i) {
      auto row=(*old)[i];
      if(Domain(d)!=Domain::Runtime && row.active!=viewActive(view,Domain(d),Kind(k),i))
        invalid("Source resource phases require their selected retirement/restore receipt");
      if(Domain(d)==Domain::Runtime && row.active && !view.runtime.isTextureActive(uint32_t(i))) {
        budget.charge(sizeof(ResourceHandle));deactivate.push_back(row.handle);row.active=false;
      } else if(!payloadMatches(row,view,Domain(d),Kind(k),i,check)) {
        if(!reconcile || !corePayloadMatches(row,view,Domain(d),Kind(k),i,check))
          invalid("Existing texture resource payload/phase changed without replay authority");
        auto replacement=makePayload(view,Domain(d),Kind(k),i,budget);
        if(Kind(k)==Kind::Texture) {
          const auto& previous=row.payload->dependencies;
          const auto& current=replacement->dependencies;
          const auto same=[](auto a,auto b) {return a.size()==b.size() && std::equal(a.begin(),a.end(),b.begin());};
          budget.visit(plus(previous.images().size(),previous.samplers().size()));
          if(!same(previous.images(),current.images()) || !same(previous.samplers(),current.samplers()))
            invalid("Texture metadata maintenance cannot replace authored image/sampler dependencies");
        }
        row.payload=std::move(replacement);
      }
      next->tables[t].push_back(std::move(row));
    }
    for(size_t i=common;i<oldCount;++i) if((*old)[i].active) {
      budget.charge(sizeof(ResourceHandle));budget.visit();deactivate.push_back((*old)[i].handle);
    }
    const auto appended=n>oldCount?n-oldCount:0;newRows=plus(newRows,appended);
    if(uint64_t(authority.high[t])+appended>UINT32_MAX) exceeded();
    state->nextHigh[t]=uint32_t(uint64_t(authority.high[t])+appended);
    for(size_t i=oldCount;i<n;++i) {
      const bool active=viewActive(view,Domain(d),Kind(k),i);
      if(before && !active) invalid("New inactive resource rows require fresh bootstrap");
      next->tables[t].push_back({{},makePayload(view,Domain(d),Kind(k),i,budget),active});
    }
  }
  // Reused source records must still refer to present rows after an explicitly
  // reconciled suffix removal. Opaque schemas remain incomplete, never an
  // unused-resource proof, while every known edge is checked exactly.
  for(const auto domain:{Domain::Archive,Domain::Evaluated}) {
    const auto& textures=next->tables[table(domain,Kind::Texture)];
    const auto images=next->tables[table(domain,Kind::Image)].size(),samplers=next->tables[table(domain,Kind::Sampler)].size();
    for(const auto& row:textures) if(row.active) {
      const auto& refs=row.payload->dependencies;
      for(const auto image:refs.images()) {budget.visit();if(image>=images || !next->tables[table(domain,Kind::Image)][image].active) invalid("Surviving source texture references an inactive/missing image");}
      for(const auto sampler:refs.samplers()) {budget.visit();if(sampler>=samplers || !next->tables[table(domain,Kind::Sampler)][sampler].active) invalid("Surviving source texture references an inactive/missing sampler");}
    }
  }
  budget.visit(limits.maxWork-check.remaining);
  if(newRows && !deactivate.empty()) invalid("Appending and retiring texture resources require separate publications");
  std::vector<ResourceRegistrationRequest> requests;budget.charge(times(newRows,sizeof(ResourceRegistrationRequest)));requests.reserve(newRows);
  for(size_t d=0;d<3;++d) for(size_t k=0;k<size_t(Kind::Count);++k) if(supported(Domain(d),Kind(k))) {
    const auto t=table(Domain(d),Kind(k));const auto oldCount=before?Access::get(*before)->tables[t].size():0;
    for(size_t i=oldCount;i<next->tables[t].size();++i)
      requests.push_back({authority.domains[d],Kind(k),uint32_t(uint64_t(authority.high[t])+i-oldCount)});
  }
  if(!requests.empty()) {
    state->registration=authority.catalog->prepareIssue(requests);
    size_t position=0;
    // prepareIssue sorts by domain, kind and internal slot; our requests use
    // precisely that same stable order.
    for(size_t d=0;d<3;++d) for(size_t k=0;k<size_t(Kind::Count);++k) if(supported(Domain(d),Kind(k))) {
      const auto t=table(Domain(d),Kind(k));const auto oldCount=before?Access::get(*before)->tables[t].size():0;
      for(size_t i=oldCount;i<next->tables[t].size();++i) next->tables[t][i].handle=state->registration.resources()[position++];
    }
  }
  if(!before) {
    // This isolated catalog has no published observers. Establish initial dead
    // runtime phases before the final externally observable pending exchange.
    if(state->registration && !authority.catalog->exchange(state->registration)) invalid("Private bootstrap registration became stale");
    state->registration={};
    std::vector<ResourceHandle> dead;budget.charge(times(total,sizeof(ResourceHandle)));dead.reserve(total);
    for(const auto& rows:next->tables) for(const auto& row:rows) if(!row.active) dead.push_back(row.handle);
    if(!dead.empty()) {auto retired=authority.catalog->prepareRetirement(dead,"Initial inactive source/runtime resource rows");
      if(!authority.catalog->exchange(retired)) invalid("Private bootstrap retirement became stale");}
    state->transition=authority.catalog->prepareDependencyChange({},"Publish initial texture resource generation");
  } else if(!deactivate.empty()) {
    state->deferredRetirement=std::move(deactivate);
    if(reconcile) state->limits.retirementPolicy=ResourceRetirementPolicy::InvalidatePreparations;
  } else {
    if(!state->registration) state->transition=authority.catalog->prepareDependencyChange({},"Publish unchanged texture resource generation");
    // Even an exact reuse/promotion command has a real endpoint and branch
    // witness. It owns no unrelated payloads when its numeric tail is empty.
    if(!reconcile) {
      budget.charge(sizeof(Access::CommandState)+128);
      auto command=std::make_shared<Access::CommandState>();command->authority=next->authority;
      if(state->registration) command->appendAuthority=authority.catalog->retainAppendAuthority(state->registration,"Original source/runtime resource append");
      for(size_t t=0;t<tableCount;++t) {
        const auto oldCount=Access::get(*before)->tables[t].size();auto& prefix=command->prefix[t];prefix.count=oldCount;
        if(oldCount) prefix.last=Access::get(*before)->tables[t].back().handle;
        budget.charge(times(next->tables[t].size()-oldCount,sizeof(Row)));
        command->tails[t].assign(next->tables[t].begin()+oldCount,next->tables[t].end());
      }
      state->command=Access::command(std::move(command));
    }
  }
  state->revision=authority.catalog->revision();next->revision=state->revision+1;
  captureRoles(*next,view,budget);
  next->bytes=plus(sizeof(Access::GenerationState)+128,generationBytes(next->tables));
  budget.charge(next->bytes);budget.charge(sizeof(Authority));budget.charge(authority.catalog->ownedBytes());
  state->next=Access::generation(std::move(next));
  if(Access::get(state->command)) {
    auto& command=*Access::get(state->command);std::vector<TextureResourceSelection> tails;
    captureRoleTails(command,*Access::get(*state->next),budget);
    for(size_t d=0;d<3;++d) for(size_t k=0;k<size_t(Kind::Count);++k) if(supported(Domain(d),Kind(k))) {
      const auto t=table(Domain(d),Kind(k));
      for(size_t i=0;i<command.tails[t].size();++i) tails.push_back({Domain(d),Kind(k),uint32_t(command.prefix[t].count+i)});
    }
    command.references=selected(Access::get(*state->next),tails,Pins::History,limits.maxOwnedBytes,false,state.get());
    command.bytes=plus(sizeof(Access::CommandState)+128,plus(tableBytes(command.tails),command.references->ownedBytes()));
    for(const auto& roles:command.roleTails) command.bytes=plus(command.bytes,times(plus(roles.color.capacity(),roles.data.capacity()),sizeof(uint32_t)));
    command.bytes=plus(command.bytes,plus(times(command.mappingTail.capacity(),sizeof(int32_t)),times(command.identityTail.capacity(),sizeof(std::shared_ptr<const uint8_t>))));
    budget.charge(command.bytes);
  }
  state->bytes=budget.bytes;auto result=Access::prepared(std::move(state));prepareAutomaticLiveOwners(result);return result;
}

PreparedTextureResourceLifecycle PrepareTextureResourceLifecycle(const TextureResourceLifecycleGenerationPtr& before,
    const TextureResourceLifecycleView& view,const TextureResourceLifecycleLimits& limits) {
  return prepareTextureResourceLifecycle(before,view,limits,false);
}
PreparedTextureResourceLifecycle ReconcileTextureResourceLifecycle(const TextureResourceLifecycleGenerationPtr& before,
    const TextureResourceLifecycleView& view,const TextureResourceLifecycleLimits& limits) {
  return prepareTextureResourceLifecycle(before,view,limits,true);
}

PreparedTextureResourceLifecycle PrepareTextureResourceLifecycleReplay(const TextureResourceLifecycleGenerationPtr& before,
    const TextureResourceLifecycleView& view,const TextureResourceLifecycleCommand& receipt,bool forward,
    const TextureResourceLifecycleLimits& limits) {
  Budget budget(limits);budget.charge(sizeof(Access::PreparedState)+sizeof(Access::GenerationState)+256);
  if(!before || !before->valid() || !before->liveDependenciesValid() || !receipt) invalid("Texture resource replay requires a current generation and published command");
  const auto& command=*Access::get(receipt);checkPrefix(command,*Access::get(*before),!forward);
  if(!forward && !roleTailsMatch(command,*Access::get(*before))) invalid("Owned source role/cookie endpoint changed before resource replay");
  if(command.version==UINT64_MAX) exceeded();
  auto state=std::make_unique<Access::PreparedState>();state->before=before;state->command=receipt;state->limits=limits;
  state->commandVersion=command.version;state->replay=true;state->forward=forward;
  auto next=std::make_shared<Access::GenerationState>();next->authority=command.authority;next->maxWork=limits.maxWork;
  auto& authority=*next->authority;state->expectedHigh=authority.high;state->nextHigh=authority.high;
  state->expectedLiveVersion=state->nextLiveVersion=authority.liveVersion;
  std::vector<ResourceHandle> changed;uint64_t rowCount=0;
  for(size_t t=0;t<tableCount;++t) {
    const auto n=command.prefix[t].count;const auto& old=Access::get(*before)->tables[t];
    rowCount=plus(rowCount,n+(forward?command.tails[t].size():0));if(rowCount>limits.maxRows) exceeded();
    budget.charge(times(n+(forward?command.tails[t].size():0),sizeof(Row)));
    next->tables[t].reserve(n+(forward?command.tails[t].size():0));
    next->tables[t].assign(old.begin(),old.begin()+n);
    if(forward) next->tables[t].insert(next->tables[t].end(),command.tails[t].begin(),command.tails[t].end());
    for(const auto& row:command.tails[t]) {budget.charge(sizeof(ResourceHandle));changed.push_back(row.handle);}
  }
  if(!tablesMatch(next->tables,view,limits.maxWork)) invalid("Texture resource replay payload does not match its original endpoint");
  if(changed.empty()) state->transition=authority.catalog->prepareDependencyChange({},"Replay exact resource reuse endpoint");
  else if(forward) {
    if(!command.appendAuthority) invalid("Texture resource restore lacks original append authority");
    state->transition=authority.catalog->prepareAppendChange(command.appendAuthority,ResourceLifecyclePhase::Active,{},limits.retirementPolicy);
    state->appendReplayAuthority=command.appendAuthority;
  } else {
    if(!command.appendAuthority) invalid("Texture resource cut lacks original append authority");
    state->deferredRetirement=std::move(changed);state->appendReplayAuthority=command.appendAuthority;
  }
  state->revision=authority.catalog->revision();next->revision=state->revision+1;
  captureRoles(*next,view,budget);
  if(forward && !roleTailsMatch(command,*next)) invalid("Restored source role/cookie endpoint differs from original resource command");
  next->bytes=plus(sizeof(Access::GenerationState)+128,generationBytes(next->tables));
  budget.charge(next->bytes);budget.charge(authority.catalog->ownedBytes());
  state->next=Access::generation(std::move(next));state->bytes=budget.bytes;
  auto result=Access::prepared(std::move(state));prepareAutomaticLiveOwners(result);return result;
}

namespace {
PreparedTextureResourceLifecycle prepareSelectedRetirement(const TextureResourceLifecycleGenerationPtr& before,
    const TextureResourceLifecycleView& view,std::span<const TextureResourceSelection> selections,
    const TextureResourceRetirementCommand& receipt,ResourceLifecyclePhase target,const TextureResourceLifecycleLimits& limits) {
  Budget budget(limits);budget.charge(sizeof(Access::PreparedState)+sizeof(Access::GenerationState)+256);
  Check maskCheck{limits.maxWork};if(!masksValid(view,maskCheck)) invalid("Source resource phases require exact zero/one masks");
  budget.visit(limits.maxWork-maskCheck.remaining);
  if(!before || !before->valid() || !before->liveDependenciesValid() ||
      (target!=ResourceLifecyclePhase::Active && target!=ResourceLifecyclePhase::Retired))
    invalid("Selected resource retirement requires a current generation and valid phase");
  const bool retiring=target==ResourceLifecyclePhase::Retired;
  const auto old=Access::get(*before);auto& authority=*old->authority;
  const bool initial=!receipt;
  std::shared_ptr<Access::RetirementState> command;
  if(initial) {
    if(!retiring || selections.empty() || selections.size()>limits.maxRows) invalid("Source retirement requires a finite nonempty selection");
    budget.charge(sizeof(Access::RetirementState)+128);
    budget.charge(times(selections.size(),sizeof(TextureResourceSelection)+sizeof(Row)+sizeof(RetirementTextureEndpoint)+sizeof(ResourceHandle)));
    budget.visit(times(selections.size(),plus(4,std::bit_width(selections.size()))));
    command=std::make_shared<Access::RetirementState>();
    command->authority=old->authority;command->selections.assign(selections.begin(),selections.end());
    std::ranges::sort(command->selections,selectionLess);
    if(std::adjacent_find(command->selections.begin(),command->selections.end())!=command->selections.end())
      invalid("Source retirement selection contains duplicate owners");
    command->rows.reserve(command->selections.size());command->textures.reserve(command->selections.size());
    const auto isSelected=[&](TextureResourceSelection selection) {
      return std::binary_search(command->selections.begin(),command->selections.end(),selection,selectionLess);
    };
    const auto captureEndpoint=[&](TextureResourceSelection selection,bool retireOwner,bool colorAffected,bool dataAffected) {
      RetirementTextureEndpoint endpoint;endpoint.selection=selection;endpoint.retireOwner=retireOwner;
      endpoint.colorAffected=colorAffected;endpoint.dataAffected=dataAffected;
      endpoint.sourceHandle=old->tables[table(selection.domain,Kind::Texture)][selection.row].handle;
      const auto& roles=old->roles[size_t(selection.domain)];endpoint.rolesKnown=roles.known;
      if(roles.known) {
        endpoint.color=roles.color[selection.row];endpoint.data=roles.data[selection.row];
        const auto& runtime=old->tables[table(Domain::Runtime,Kind::RuntimeTexture)];
        if(colorAffected && endpoint.color!=UINT32_MAX) endpoint.colorHandle=runtime[endpoint.color].handle;
        if(dataAffected && endpoint.data!=UINT32_MAX) endpoint.dataHandle=runtime[endpoint.data].handle;
      }
      if(selection.domain==Domain::Archive) {
        endpoint.mappingKnown=retireOwner && selection.row<old->archiveToEvaluated.size();
        if(endpoint.mappingKnown) {
          endpoint.mapping=old->archiveToEvaluated[selection.row];
          if(endpoint.mapping>=0) endpoint.mappedHandle=old->tables[table(Domain::Evaluated,Kind::Texture)][size_t(endpoint.mapping)].handle;
        }
        endpoint.cookieKnown=selection.row<old->archiveTextureIdentities.size();
        if(endpoint.cookieKnown) endpoint.cookie=old->archiveTextureIdentities[selection.row];
      }
      command->textures.push_back(std::move(endpoint));
    };
    for(const auto selection:command->selections) {
      budget.visit();
      if(!supported(selection.domain,selection.kind))
        invalid("Selected retirement accepts only source Image/Texture/Sampler or explicit RuntimeTexture rows");
      const auto& rows=old->tables[table(selection.domain,selection.kind)];
      if(selection.row>=rows.size() || !rows[selection.row].active) invalid("Selected retirement targets a missing or inactive source row");
      command->rows.push_back(rows[selection.row]);
      if(selection.kind==Kind::Texture) captureEndpoint(selection,true,true,true);
    }
    const auto scanRoles=[&](bool capture) {
      uint64_t count=0;
      for(const auto domain:{Domain::Archive,Domain::Evaluated}) {
        const auto& roles=old->roles[size_t(domain)];if(!roles.known) continue;
        const auto& rows=old->tables[table(domain,Kind::Texture)];
        for(uint32_t row=0;row<rows.size();++row) {
          budget.visit(plus(2,times(3,std::bit_width(command->selections.size()+1))));
          const TextureResourceSelection owner{domain,Kind::Texture,row};if(!rows[row].active || isSelected(owner)) continue;
          const bool color=isSelected({Domain::Runtime,Kind::RuntimeTexture,roles.color[row]});
          const bool data=isSelected({Domain::Runtime,Kind::RuntimeTexture,roles.data[row]});
          if(color || data) {++count;if(capture) captureEndpoint(owner,false,color,data);}
        }
      }
      return count;
    };
    const bool runtimeSelected=command->selections.back().domain==Domain::Runtime;
    const auto extra=runtimeSelected?scanRoles(false):0;budget.charge(times(extra,sizeof(RetirementTextureEndpoint)+128));
    command->textures.reserve(command->textures.size()+extra);if(runtimeSelected) scanRoles(true);
    budget.visit(times(command->textures.size(),plus(4,std::bit_width(command->textures.size()))));
    std::ranges::sort(command->textures,[](const auto& a,const auto& b) {return selectionLess(a.selection,b.selection);});
    const auto historyCount=plus(command->selections.size(),times(command->textures.size(),4));
    budget.charge(times(historyCount,sizeof(TextureResourceSelection)));std::vector<TextureResourceSelection> history;history.reserve(historyCount);
    history.assign(command->selections.begin(),command->selections.end());
    for(const auto& endpoint:command->textures) {
      if(!endpoint.retireOwner) history.push_back(endpoint.selection);
      if(endpoint.colorAffected && endpoint.color!=UINT32_MAX) history.push_back({Domain::Runtime,Kind::RuntimeTexture,endpoint.color});
      if(endpoint.dataAffected && endpoint.data!=UINT32_MAX) history.push_back({Domain::Runtime,Kind::RuntimeTexture,endpoint.data});
      if(endpoint.mapping>=0) history.push_back({Domain::Evaluated,Kind::Texture,uint32_t(endpoint.mapping)});
    }
    budget.visit(times(history.size(),plus(4,std::bit_width(history.size()))));std::ranges::sort(history,selectionLess);
    history.erase(std::unique(history.begin(),history.end()),history.end());
    command->references=selected(old,history,Pins::History,limits.maxOwnedBytes-budget.bytes,false);
    command->bytes=plus(sizeof(Access::RetirementState)+128,plus(
        times(command->selections.capacity(),sizeof(TextureResourceSelection)),plus(times(command->rows.capacity(),sizeof(Row)),
        plus(times(command->textures.capacity(),sizeof(RetirementTextureEndpoint)),command->references->ownedBytes()))));
    budget.charge(command->references->ownedBytes());
  } else {
    command=Access::get(receipt);
    if(command->authority!=old->authority || !command->published || !command->retirement || command->retired==retiring ||
        command->version==UINT64_MAX || !retirementEndpointMatches(*command,*old,!command->retired))
      invalid("Selected resource replay lacks its exact original phase/payload/role authority");
    budget.charge(command->bytes);
  }
  auto state=std::make_unique<Access::PreparedState>();state->before=before;state->limits=limits;
  state->retirementCommand=Access::retirement(command);state->retirementVersion=command->version;
  state->retirementInitial=initial;state->retirementTarget=retiring;
  state->expectedHigh=state->nextHigh=authority.high;state->expectedLiveVersion=state->nextLiveVersion=authority.liveVersion;
  auto next=std::make_shared<Access::GenerationState>();next->authority=old->authority;next->maxWork=limits.maxWork;
  uint64_t total=0;
  for(size_t d=0;d<3;++d) for(size_t k=0;k<size_t(Kind::Count);++k) if(supported(Domain(d),Kind(k))) {
    const auto t=table(Domain(d),Kind(k));const auto& rows=old->tables[t];
    total=plus(total,rows.size());if(total>limits.maxRows || rows.size()!=count(view,Domain(d),Kind(k)))
      invalid("Selected resource phase change must preserve every stable table address");
    budget.charge(times(rows.size(),sizeof(Row)));budget.visit(rows.size());next->tables[t]=rows;
  }
  budget.charge(times(command->selections.size(),sizeof(ResourceHandle)));std::vector<ResourceHandle> changed;changed.reserve(command->selections.size());
  for(size_t i=0;i<command->selections.size();++i) {
    const auto selection=command->selections[i];auto& row=next->tables[table(selection.domain,selection.kind)][selection.row];
    if(row.handle!=command->rows[i].handle || row.payload!=command->rows[i].payload || row.active==!retiring)
      invalid("Selected resource endpoint belongs to another phase or incarnation");
    row.active=!retiring;changed.push_back(row.handle);
  }
  if(!tablesMatch(next->tables,view,limits.maxWork)) invalid("Selected resource phase change modifies records or leaves an inactive dependency");
  captureRoles(*next,view,budget);
  uint64_t roleRows=next->archiveToEvaluated.size();for(const auto& roles:next->roles) roleRows=plus(roleRows,roles.color.size());
  budget.visit(plus(command->selections.size(),times(roleRows,plus(2,std::bit_width(command->textures.size()+1)))));
  if(!retirementRolesChangeOnlySelected(*command,*old,*next,!retiring))
    invalid("Selected resource change must preserve unrelated roles/mappings/cookies and restore its exact endpoint");
  if(retiring) {
    state->deferredRetirement=std::move(changed);if(!initial) state->replayAuthority=command->retirement;
  } else {
    state->transition=authority.catalog->prepareChange(command->retirement,ResourceLifecyclePhase::Active,limits.retirementPolicy);
    state->replayAuthority=command->retirement;
  }
  state->revision=authority.catalog->revision();next->revision=state->revision+1;
  next->bytes=plus(sizeof(Access::GenerationState)+128,generationBytes(next->tables));
  budget.charge(next->bytes);budget.charge(authority.catalog->ownedBytes());
  state->next=Access::generation(std::move(next));state->bytes=budget.bytes;
  auto result=Access::prepared(std::move(state));prepareAutomaticLiveOwners(result);return result;
}
}
PreparedTextureResourceLifecycle PrepareTextureResourceRetirement(const TextureResourceLifecycleGenerationPtr& before,
    const TextureResourceLifecycleView& view,std::span<const TextureResourceSelection> selections,const TextureResourceLifecycleLimits& limits) {
  return prepareSelectedRetirement(before,view,selections,{},ResourceLifecyclePhase::Retired,limits);
}
PreparedTextureResourceLifecycle PrepareTextureResourceRetirementReplay(const TextureResourceLifecycleGenerationPtr& before,
    const TextureResourceLifecycleView& view,const TextureResourceRetirementCommand& receipt,ResourceLifecyclePhase target,
    const TextureResourceLifecycleLimits& limits) {
  if(!receipt) invalid("Selected resource replay requires its original retirement receipt");
  return prepareSelectedRetirement(before,view,{},receipt,target,limits);
}

bool CanPrepareTextureResourceLifecycle(const PreparedTextureResourceLifecycle& prepared,
    const TextureResourceLifecycleGenerationPtr& current,const TextureResourceLifecycleView& nextView) noexcept {
  const auto& pointer=Access::get(prepared);if(!pointer || pointer->consumed || pointer->failed || current!=pointer->before || !pointer->next) return false;
  const auto& state=*pointer;const auto& next=*Access::get(*state.next);const auto& authority=*next.authority;
  if(authority.catalog->revision()!=state.revision || authority.high!=state.expectedHigh || authority.liveVersion!=state.expectedLiveVersion ||
      (current && (!current->valid() || !current->liveDependenciesValid())) || !roleViewMatches(next,nextView) || !tablesMatch(next.tables,nextView,next.maxWork)) return false;
  for(const auto& owner:state.outgoing) {
    if(!owner || !owner->active() || Access::get(*owner)->authority!=next.authority) return false;
    const auto slot=authority.liveSlots.find(liveLabel(owner->key()));if(slot==authority.liveSlots.end() || slot->second.active.lock()!=owner) return false;
  }
  for(const auto& owner:state.incoming) {
    if(!owner || !owner->pending() || !owner->matches(state.next)) return false;
    const auto slot=authority.liveSlots.find(liveLabel(owner->key()));if(slot==authority.liveSlots.end() || !slot->second.reservations) return false;
    const auto activeOwner=slot->second.active.lock();const auto predecessor=Access::get(*owner)->predecessor.lock();
    if(activeOwner && activeOwner->active() && activeOwner!=predecessor) return false;
  }
  for(const auto& owner:next.sourceOwners) {
    if(!prepared.liveOwnerWillBeActive(owner)) return false;
  }
  if(state.command && state.replay) {
    const auto& command=*Access::get(state.command);
    if(command.version!=state.commandVersion || command.applied==state.forward) return false;
    // All checks that can throw were performed at preparation. The immutable
    // prefix boundary and exact tail handles are sufficient for append-only
    // branch lineage: changing any earlier row requires replacing its suffix.
    for(size_t t=0;t<tableCount;++t) {
      const auto& prefix=command.prefix[t];const auto& rows=Access::get(*current)->tables[t];
      if(rows.size()!=prefix.count+(command.applied?command.tails[t].size():0) ||
          (prefix.count && rows[prefix.count-1].handle!=prefix.last)) return false;
      if(command.applied) for(size_t i=0;i<command.tails[t].size();++i)
        if(rows[prefix.count+i].handle!=command.tails[t][i].handle || rows[prefix.count+i].payload!=command.tails[t][i].payload ||
            rows[prefix.count+i].active!=command.tails[t][i].active) return false;
    }
  }
  if(state.retirementCommand) {
    const auto& command=*Access::get(state.retirementCommand);
    if(!current || command.authority!=next.authority || command.version!=state.retirementVersion ||
        command.retired==state.retirementTarget || command.published==state.retirementInitial ||
        !retirementEndpointMatches(command,*Access::get(*current),!command.retired) ||
        !retirementRolesChangeOnlySelected(command,*Access::get(*current),next,!state.retirementTarget)) return false;
  }
  return true;
}
bool CanExchangeTextureResourceLifecycle(const PreparedTextureResourceLifecycle& prepared,
    const TextureResourceLifecycleGenerationPtr& current,const TextureResourceLifecycleView& nextView) noexcept {
  if(!prepared.sealed() || !CanPrepareTextureResourceLifecycle(prepared,current,nextView)) return false;
  const auto& state=*Access::get(prepared);const auto& catalog=*Access::get(*state.next)->authority->catalog;
  return state.registration?catalog.canExchange(state.registration):catalog.canExchange(state.transition);
}
bool ExchangeTextureResourceLifecycle(PreparedTextureResourceLifecycle& prepared,
    TextureResourceLifecycleGenerationPtr& current,const TextureResourceLifecycleView& nextView) noexcept {
  if(!CanExchangeTextureResourceLifecycle(prepared,current,nextView)) return false;
  auto& state=*Access::get(prepared);auto& next=*Access::get(*state.next);auto& authority=*next.authority;
  if(state.registration) {if(!authority.catalog->exchange(state.registration)) return false;}
  else if(!authority.catalog->exchange(state.transition)) return false;
  authority.high=state.nextHigh;authority.liveVersion=state.nextLiveVersion;next.published=true;next.revision=authority.catalog->revision();
  for(const auto& owner:state.outgoing) {
    auto& live=*Access::get(*owner);live.enabled=false;live.cancelled=true;if(live.blocker) live.blocker->cancel();
    const auto slot=authority.liveSlots.find(liveLabel(live.key));if(slot!=authority.liveSlots.end()) slot->second.active.reset();
  }
  for(const auto& owner:state.incoming) {
    auto& live=*Access::get(*owner);live.pending=false;live.enabled=true;
    const auto slot=authority.liveSlots.find(liveLabel(live.key));assert(slot!=authority.liveSlots.end() && slot->second.reservations);
    slot->second.active=owner;--slot->second.reservations;
  }
  if(Access::get(state.command)) {
    auto& command=*Access::get(state.command);command.published=true;command.applied=state.replay?state.forward:true;
    ++command.version;
  }
  if(Access::get(state.retirementCommand)) {
    auto& command=*Access::get(state.retirementCommand);command.published=true;command.retired=state.retirementTarget;
    if(state.replayAuthority) command.retirement=state.replayAuthority;
    ++command.version;
  }
  current.swap(state.next);state.consumed=true;return true;
}

TextureResourceReferenceOwnerPtr RetainTextureResourceReferences(const TextureResourceLifecycleGenerationPtr& generation,
    std::span<const TextureResourceSelection> input,Pins kind,uint64_t maxBytes,bool opaque) {
  if(!generation || !generation->valid()) invalid("Selected texture resource generation is stale or empty");
  return selected(Access::get(*generation),input,kind,maxBytes,opaque);
}
TextureResourceReferenceOwnerPtr RetainAllTextureResourceReferences(const TextureResourceLifecycleGenerationPtr& generation,Pins kind,uint64_t maxBytes) {
  if(!generation || !generation->valid()) invalid("Full texture resource generation is stale or empty");
  const auto selections=allSelections(*Access::get(*generation),maxBytes);
  return RetainTextureResourceReferences(generation,selections,kind,maxBytes);
}
TextureResourceReferenceOwnerPtr CombineTextureResourceReferences(std::span<const TextureResourceReferenceOwnerPtr> owners,uint64_t maxBytes) {
  TextureResourceLifecycleLimits limits;limits.maxOwnedBytes=maxBytes;Budget budget(limits);
  budget.charge(sizeof(Access::OwnerState)+sizeof(TextureResourceReferenceOwner)+256);
  auto result=std::make_shared<Access::OwnerState>();
  if(owners.size()>4096) exceeded();
  for(const auto& owner:owners) {
    if(!owner || !owner->valid()) invalid("Combined texture resource owner is stale or empty");
    const auto& input=*Access::get(*owner);
    for(const auto& authority:input.authorities) {
      budget.visit(result->authorities.size()+1);
      if(std::ranges::find(result->authorities,authority)==result->authorities.end()) {
        budget.charge(sizeof(std::shared_ptr<Authority>));result->authorities.push_back(authority);
      }
    }
    for(const auto& row:input.rows) {
      budget.visit(result->rows.size()+1);
      if(std::ranges::none_of(result->rows,[&](const auto& old){return old.authority==row.authority && old.row.handle==row.row.handle;})) {
        budget.charge(sizeof(SelectedRow)+(row.row.payload?row.row.payload->bytes:0));result->rows.push_back(row);
      }
    }
    for(const auto& pin:input.pins) {budget.visit(result->pins.size()+1);if(std::ranges::find(result->pins,pin)==result->pins.end()) {
      budget.charge(sizeof(std::shared_ptr<PinControl>)+128);result->pins.push_back(pin);
    }}
    for(const auto& blocker:input.blockers) {budget.visit(result->blockers.size()+1);if(std::ranges::find(result->blockers,blocker)==result->blockers.end()) {
      budget.charge(sizeof(std::shared_ptr<OpaqueBlocker>)+128);result->blockers.push_back(blocker);
    }}
  }
  finishOwner(*result,budget);return Access::owner(std::move(result));
}
TextureResourceReferenceOwnerPtr AddTextureResourceReferencePin(const TextureResourceReferenceOwnerPtr& owner,
    ResourcePin&& pin,uint64_t maxBytes) {
  if(!owner || !owner->valid() || !pin) invalid("Selected resource/material pin ownership is empty or stale");
  TextureResourceLifecycleLimits limits;limits.maxOwnedBytes=maxBytes;Budget budget(limits);
  budget.charge(plus(owner->ownedBytes(),sizeof(Access::OwnerState)+sizeof(TextureResourceReferenceOwner)+256));
  auto result=std::make_shared<Access::OwnerState>();const auto& input=*Access::get(*owner);
  result->authorities=input.authorities;result->rows=input.rows;result->pins=input.pins;result->blockers=input.blockers;
  // This API joins material history authority, never an unchecked foreign
  // resource catalog capability that could masquerade as a selected row.
  for(const auto& resource:pin.resources()) if(resource.kind()!=Kind::Material) invalid("Additional selected endpoint pin must contain only materials");
  result->pins.push_back(std::make_shared<PinControl>(std::move(pin)));
  finishOwner(*result,budget);return Access::owner(std::move(result));
}

} // namespace container::scene
