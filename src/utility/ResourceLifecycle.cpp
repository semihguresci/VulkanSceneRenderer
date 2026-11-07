#include "Container/utility/ResourceLifecycle.h"

#include <algorithm>
#include <bit>
#include <limits>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <utility>

namespace container::util {
namespace resource_lifecycle_detail {
constexpr size_t kinds = size_t(ResourceKind::Count), classes = size_t(ResourcePinClass::Count);
[[noreturn]] void fail(const char* reason) { throw std::invalid_argument(reason); }
[[noreturn]] void limit(const char* reason) { throw std::length_error(reason); }
uint64_t add(uint64_t a,uint64_t b) {
  if(b>UINT64_MAX-a) limit("Resource lifecycle byte arithmetic overflow");
  return a+b;
}
uint64_t mul(uint64_t a,uint64_t b) {
  if(a && b>UINT64_MAX/a) limit("Resource lifecycle byte arithmetic overflow");
  return a*b;
}
bool valid(ResourceKind value) noexcept {return size_t(value)<kinds;}
bool valid(ResourceDomainKind value) noexcept {return size_t(value)<size_t(ResourceDomainKind::Count);}
bool valid(ResourcePinClass value) noexcept {return size_t(value)<classes;}
bool valid(ResourceLifecyclePhase value) noexcept {return value==ResourceLifecyclePhase::Active || value==ResourceLifecyclePhase::Retired;}
bool valid(ResourceRetirementPolicy value) noexcept {
  return value==ResourceRetirementPolicy::BlockPreparations || value==ResourceRetirementPolicy::InvalidatePreparations;
}
struct Domain {
  std::shared_ptr<const uint8_t> owner;
  std::weak_ptr<Catalog> accounting;
  size_t index{};
  uint64_t serial{},charge{};
  ResourceDomainKind kind{};
  ~Domain();
};
struct Handle {
  std::shared_ptr<const Domain> domain;
  std::weak_ptr<Catalog> accounting;
  ResourceKind kind{};
  uint32_t slot{};
  uint64_t incarnation{},charge{};
  ~Handle();
};
struct Cell {
  ResourceHandle handle;
  bool active{true};
  std::array<uint64_t,classes> pins{};
  // phaseRevision never rewinds. endpointRevision can be restored ONLY by an
  // original registration's owned suspension/resume roundtrip.
  uint64_t commands{},phaseRevision{1},endpointRevision{1},retiredBy{};
};
struct DomainRows {
  ResourceLifecycleDomain domain;
  std::array<std::vector<Cell>,kinds> tables;
};
struct OutgoingPin {
  Pin* pin{};
  uint64_t serial{};
  std::vector<ResourceHandle> handles;
};
struct Catalog : std::enable_shared_from_this<Catalog> {
  ResourceLifecycleLimits limits;
  std::shared_ptr<const uint8_t> owner{std::make_shared<const uint8_t>(0)};
  bool closed{};
  uint64_t revision{1},nextDomain{1},nextIncarnation{1},nextPin{1},nextCommand{1};
  uint64_t bytes{sizeof(Catalog)},slots{},pinSets{},pinReferences{},commandReferences{},snapshots{},snapshotRows{};
  uint64_t registrations{},registrationReferences{};
  std::vector<DomainRows> domains;
  std::vector<Pin*> pins;
  std::vector<Command*> commands;
  std::vector<Registration*> registrationOwners;
  std::vector<Transition*> transitionOwners;
  std::vector<OutgoingPin> captureOutgoing(std::span<const ResourcePin* const>);
  void open() const {if(closed) fail("Resource lifecycle owner is closed");}
  void room(uint64_t extra) const {
    if(bytes>limits.maxOwnedBytes || extra>limits.maxOwnedBytes-bytes) limit("Resource lifecycle aggregate owned-byte limit exceeded");
  }
  void work(uint64_t amount) const {if(amount>limits.maxWork) limit("Resource lifecycle operation work limit exceeded");}
  void reason(std::string_view value) const {
    if(value.empty() || value.size()>limits.maxReasonBytes) fail("Resource lifecycle pin reason is empty or exceeds its bound");
  }
  DomainRows* domain(const ResourceLifecycleDomain& value) noexcept {
    if(!value.state_ || value.state_->owner!=owner || value.state_->index>=domains.size()) return nullptr;
    auto& row=domains[value.state_->index];
    return row.domain==value?&row:nullptr;
  }
  const DomainRows* domain(const ResourceLifecycleDomain& value) const noexcept {
    return const_cast<Catalog*>(this)->domain(value);
  }
  Cell* cell(const ResourceHandle& value) noexcept {
    if(!value.state_ || !valid(value.state_->kind)) return nullptr;
    ResourceLifecycleDomain d;d.state_=value.state_->domain;
    auto* rows=domain(d);if(!rows) return nullptr;
    auto& table=rows->tables[size_t(value.state_->kind)];
    if(value.state_->slot>=table.size()) return nullptr;
    auto& row=table[value.state_->slot];
    return row.handle==value?&row:nullptr;
  }
  const Cell* cell(const ResourceHandle& value) const noexcept {return const_cast<Catalog*>(this)->cell(value);}
  static bool reclaimable(const Cell& row) noexcept {
    return !row.active && !row.commands && std::ranges::all_of(row.pins,[](uint64_t n){return n==0;});
  }
};
struct TemporaryCharge {
  Catalog& owner;
  uint64_t bytes{};
  TemporaryCharge(Catalog& value,uint64_t amount):owner(value),bytes(amount) {owner.room(bytes);owner.bytes+=bytes;}
  ~TemporaryCharge() {owner.bytes-=bytes;}
  uint64_t release() noexcept {return std::exchange(bytes,0);}
};
Domain::~Domain() {if(charge) if(auto state=accounting.lock()) state->bytes-=charge;}
Handle::~Handle() {if(charge) if(auto state=accounting.lock()) state->bytes-=charge;}
bool lessHandle(const ResourceHandle& a,const ResourceHandle& b) noexcept {
  return std::tuple(a.domain().identity(),a.kind(),a.slot(),a.incarnation())<
      std::tuple(b.domain().identity(),b.kind(),b.slot(),b.incarnation());
}
uint64_t normalizationWork(size_t count) {return mul(count,add(8,mul(4,std::bit_width(count))));}
std::vector<ResourceHandle> normalize(Catalog& state,std::span<const ResourceHandle> input) {
  if(input.empty()) fail("Resource lifecycle inventory is empty");
  state.work(normalizationWork(input.size()));state.room(mul(input.size(),sizeof(ResourceHandle)));
  for(const auto& handle:input) if(!state.cell(handle)) fail("Resource lifecycle handle is stale or foreign");
  std::vector<ResourceHandle> result(input.begin(),input.end());
  std::ranges::sort(result,lessHandle);result.erase(std::unique(result.begin(),result.end()),result.end());
  return result;
}
struct Pin {
  enum class Phase : uint8_t { Pending, Active, Cancelled };
  std::shared_ptr<Catalog> owner;
  ResourcePinClass kind{};
  std::string reason;
  std::vector<ResourceHandle> handles;
  uint64_t serial{},charge{};
  Phase phase{Phase::Active};
  Registration* registration{};
  Transition* transition{};
  bool registered{};
  ~Pin();
};
struct Command {
  std::shared_ptr<Catalog> owner;
  std::vector<ResourceHandle> handles;
  std::vector<uint64_t> phaseRevisions;
  std::vector<uint64_t> resumeEndpoints;
  std::string reason;
  Registration* registration{};
  uint64_t serial{},charge{},version{};
  ResourceLifecyclePhase phase{ResourceLifecyclePhase::Active};
  bool registered{},published{},append{};
  ~Command() {
    if(registered) {
      if(!append || published) for(const auto& handle:handles) if(auto* row=owner->cell(handle)) --row->commands;
      owner->commandReferences-=handles.size();
      const auto found=std::ranges::find(owner->commands,this);if(found!=owner->commands.end()) owner->commands.erase(found);
    }
    if(charge) owner->bytes-=charge;
  }
};
struct Transition {
  std::shared_ptr<Catalog> owner;
  ResourceRetirementCommand command;
  std::shared_ptr<Command> append;
  std::vector<uint64_t> appendPhases,appendEndpoints;
  ResourcePin preparation;
  std::vector<OutgoingPin> outgoing;
  std::vector<Pin*> pendingPins;
  uint64_t revision{},commandVersion{},charge{};
  uint64_t stagedReferences{};
  ResourceLifecyclePhase target{};
  ResourceRetirementPolicy policy{};
  bool registered{},consumed{};
  void cancelPins() noexcept {
    for(auto* pin:pendingPins) {
      pin->transition=nullptr;pin->phase=Pin::Phase::Cancelled;pin->registered=false;
      --owner->pinSets;owner->pinReferences-=pin->handles.size();
    }
    pendingPins.clear();stagedReferences=0;
  }
  ~Transition() {
    cancelPins();
    if(registered) {
      const auto found=std::ranges::find(owner->transitionOwners,this);
      if(found!=owner->transitionOwners.end()) owner->transitionOwners.erase(found);
    }
    if(charge) owner->bytes-=charge;
  }
};
struct Registration {
  std::shared_ptr<Catalog> owner;
  std::vector<ResourceHandle> handles;
  std::vector<OutgoingPin> outgoing;
  std::vector<Pin*> pendingPins;
  std::shared_ptr<Command> append;
  uint64_t revision{},charge{},newCells{},stagedReferences{};
  bool registered{},consumed{};
  void cancelPins() noexcept {
    for(auto* pin:pendingPins) {
      pin->registration=nullptr;pin->phase=Pin::Phase::Cancelled;pin->registered=false;
      --owner->pinSets;owner->pinReferences-=pin->handles.size();
    }
    pendingPins.clear();stagedReferences=0;
  }
  void cancelAppend() noexcept {
    if(!append || append->registration!=this) return;
    append->registration=nullptr;append->registered=false;
    owner->commandReferences-=append->handles.size();
    const auto found=std::ranges::find(owner->commands,append.get());if(found!=owner->commands.end()) owner->commands.erase(found);
  }
  ~Registration() {
    if(!registered) return;
    cancelPins();cancelAppend();
    const auto found=std::ranges::find(owner->registrationOwners,this);
    if(found!=owner->registrationOwners.end()) owner->registrationOwners.erase(found);
    owner->bytes-=charge;--owner->registrations;owner->registrationReferences-=handles.size();
  }
};
Pin::~Pin() {
  if(registered) {
    if(phase==Phase::Active) {
      for(const auto& handle:handles) if(auto* row=owner->cell(handle)) --row->pins[size_t(kind)];
      const auto found=std::ranges::find(owner->pins,this);if(found!=owner->pins.end()) owner->pins.erase(found);
    } else if(phase==Phase::Pending && registration) {
      registration->stagedReferences-=handles.size();
      const auto found=std::ranges::find(registration->pendingPins,this);
      if(found!=registration->pendingPins.end()) registration->pendingPins.erase(found);
    } else if(phase==Phase::Pending && transition) {
      transition->stagedReferences-=handles.size();
      const auto found=std::ranges::find(transition->pendingPins,this);
      if(found!=transition->pendingPins.end()) transition->pendingPins.erase(found);
    }
    --owner->pinSets;owner->pinReferences-=handles.size();
  }
  if(charge) owner->bytes-=charge;
}
bool prospective(const Registration& registration,const ResourceHandle& handle) noexcept {
  const auto found=std::lower_bound(registration.handles.begin(),registration.handles.end(),handle,lessHandle);
  return found!=registration.handles.end() && *found==handle;
}
bool currentOrProspective(const Catalog& owner,const Registration& registration,const ResourceHandle& handle) noexcept {
  const auto* row=owner.cell(handle);return (row && row->active) || prospective(registration,handle);
}
struct Snapshot {
  std::shared_ptr<Catalog> owner;
  ResourcePin retention;
  uint64_t revision{},charge{};
  std::vector<ResourceInventoryRow> rows;
  std::vector<ResourceInventoryReason> reasons;
  bool registered{};
  ~Snapshot() {
    if(registered) {owner->bytes-=charge;--owner->snapshots;owner->snapshotRows-=rows.size();}
  }
};
std::vector<OutgoingPin> Catalog::captureOutgoing(std::span<const ResourcePin* const> input) {
  // Registry membership is checked before inspecting a captured Pin pointer.
  // A serial prevents allocator address reuse from replacing its authority.
  uint64_t references=0;
  work(add(mul(input.size(),add(pinSets,1)),normalizationWork(input.size())));
  for(const auto* token:input) {
    const auto* pin=token?token->state_.get():nullptr;
    if(!pin || std::ranges::find(pins,pin)==pins.end() || !pin->registered ||
        pin->phase!=Pin::Phase::Active || pin->owner.get()!=this || pin->kind!=ResourcePinClass::LiveDependency)
      fail("Outgoing dependency owner is inactive, pending, foreign or has the wrong class");
    references=add(references,pin->handles.size());
  }
  if(input.size()>limits.maxPinSets || references>limits.maxPinnedReferences)
    limit("Resource lifecycle outgoing dependency limit exceeded");
  work(add(mul(input.size(),add(pinSets,1)),add(references,normalizationWork(input.size()))));
  room(add(mul(input.size(),sizeof(OutgoingPin)),mul(references,sizeof(ResourceHandle))));
  std::vector<OutgoingPin> result;result.reserve(input.size());
  for(const auto* token:input) {
    const auto& pin=*token->state_;
    for(const auto& handle:pin.handles) {
      const auto* row=cell(handle);
      if(!row || !row->active || !row->pins[size_t(ResourcePinClass::LiveDependency)])
        fail("Outgoing dependency owner no longer names active exact rows");
    }
    result.push_back({token->state_.get(),pin.serial,pin.handles});
  }
  std::ranges::sort(result,{},&OutgoingPin::serial);
  for(size_t i=1;i<result.size();++i) if(result[i-1].serial==result[i].serial)
    fail("Outgoing dependency owner is repeated");
  return result;
}
uint64_t outgoingBytes(const std::vector<OutgoingPin>& pins) {
  // Invisible reserve capacity is padding; logical proof rows are charged.
  uint64_t result=mul(pins.size(),sizeof(OutgoingPin));
  for(const auto& pin:pins) result=add(result,mul(pin.handles.capacity(),sizeof(ResourceHandle)));
  return result;
}
uint64_t transitionWork(const Catalog& owner,std::span<const OutgoingPin> outgoing,size_t rows,uint64_t staged=0) {
  uint64_t references=0,lookup=0;
  for(const auto& pin:outgoing) {
    references=add(references,pin.handles.size());lookup=add(lookup,add(1,std::bit_width(pin.handles.size())));
  }
  return add(add(mul(outgoing.size(),add(owner.pinSets,1)),references),add(mul(rows,add(lookup,1)),staged));
}
bool outgoingMatches(const Catalog& owner,std::span<const OutgoingPin> outgoing) noexcept {
  for(const auto& proof:outgoing) {
    const auto found=std::ranges::find(owner.pins,proof.pin);
    if(found==owner.pins.end()) return false;
    const auto& pin=**found;
    if(!pin.registered || pin.phase!=Pin::Phase::Active || pin.owner.get()!=&owner ||
        pin.kind!=ResourcePinClass::LiveDependency || pin.serial!=proof.serial || pin.handles!=proof.handles) return false;
    for(const auto& handle:pin.handles) {
      const auto* row=owner.cell(handle);
      if(!row || !row->active || !row->pins[size_t(ResourcePinClass::LiveDependency)]) return false;
    }
  }
  return true;
}
uint64_t releasedCount(std::span<const OutgoingPin> outgoing,const ResourceHandle& handle) noexcept {
  uint64_t result=0;
  for(const auto& pin:outgoing)
    if(std::binary_search(pin.handles.begin(),pin.handles.end(),handle,lessHandle)) ++result;
  return result;
}
bool transitionHandle(const Catalog& owner,const Transition& transition,std::span<const ResourceHandle> selected,
    const ResourceHandle& handle,ResourcePinClass kind) noexcept {
  const auto* row=owner.cell(handle);if(!row) return false;
  const bool chosen=std::binary_search(selected.begin(),selected.end(),handle,lessHandle);
  if(chosen) return kind!=ResourcePinClass::LiveDependency || transition.target==ResourceLifecyclePhase::Active;
  return row->active;
}
std::span<const ResourceHandle> transitionResources(const Transition& transition) noexcept {
  return transition.append?std::span<const ResourceHandle>(transition.append->handles):transition.command.resources();
}
void releaseOutgoing(Catalog& owner,std::span<const OutgoingPin> outgoing) noexcept {
  for(const auto& proof:outgoing) {
    auto& pin=*proof.pin;
    for(const auto& handle:pin.handles) --owner.cell(handle)->pins[size_t(ResourcePinClass::LiveDependency)];
    const auto found=std::ranges::find(owner.pins,&pin);owner.pins.erase(found);
    pin.registered=false;pin.phase=Pin::Phase::Cancelled;
    --owner.pinSets;owner.pinReferences-=pin.handles.size();
  }
}
bool transitionReady(const Catalog& owner,const Transition& s,const Command* c,
    std::span<const OutgoingPin> outgoing,std::span<const OutgoingPin> additional={}) noexcept {
  if(s.consumed || !s.registered || s.owner.get()!=&owner || owner.closed || s.revision!=owner.revision ||
      owner.revision==UINT64_MAX || !outgoingMatches(owner,outgoing) || !outgoingMatches(owner,additional) ||
      owner.pins.capacity()<owner.pins.size()+s.pendingPins.size()) return false;
  for(const auto* pin:s.pendingPins) {
    if(!pin || !pin->registered || pin->phase!=Pin::Phase::Pending || pin->transition!=&s ||
        pin->registration || pin->owner.get()!=&owner || !valid(pin->kind)) return false;
    for(const auto& handle:pin->handles)
      if(!transitionHandle(owner,s,transitionResources(s),handle,pin->kind)) return false;
  }
  if(s.append) {
    const auto& append=*s.append;
    if(c || append.owner.get()!=&owner || !append.append || !append.registered || !append.published || append.registration ||
        append.phase==s.target || append.version!=s.commandVersion || append.version==UINT64_MAX || !s.preparation ||
        s.preparation.resources().size()!=append.handles.size() || s.appendPhases.size()!=append.handles.size() ||
        s.appendEndpoints.size()!=append.handles.size() || append.phaseRevisions.size()!=append.handles.size() ||
        append.resumeEndpoints.size()!=append.handles.size()) return false;
    for(size_t i=0;i<append.handles.size();++i) {
      const auto* row=owner.cell(append.handles[i]);
      if(!row || row->phaseRevision!=s.appendPhases[i] || row->endpointRevision!=s.appendEndpoints[i] ||
          row->phaseRevision==UINT64_MAX || row->active!=(append.phase==ResourceLifecyclePhase::Active) ||
          row->pins[size_t(ResourcePinClass::Preparation)]==0 || (!row->active &&
              (row->retiredBy!=append.serial || row->phaseRevision!=append.phaseRevisions[i] ||
               !append.resumeEndpoints[i] || append.resumeEndpoints[i]>row->phaseRevision))) return false;
      if(s.target==ResourceLifecyclePhase::Retired &&
          (row->pins[size_t(ResourcePinClass::LiveDependency)]!=releasedCount(outgoing,append.handles[i])+releasedCount(additional,append.handles[i]) ||
           (s.policy==ResourceRetirementPolicy::BlockPreparations && row->pins[size_t(ResourcePinClass::Preparation)]!=1))) return false;
    }
    return true;
  }
  if(!c) return !s.preparation;
  if(c->owner.get()!=&owner || !c->registered || c->version!=s.commandVersion || c->version==UINT64_MAX ||
      c->phase==s.target || !s.preparation || s.preparation.resources().size()!=c->handles.size()) return false;
  for(size_t i=0;i<c->handles.size();++i) {
    const auto* row=owner.cell(c->handles[i]);
    if(!row || c->append || row->endpointRevision!=c->phaseRevisions[i] || row->phaseRevision==UINT64_MAX ||
        row->active!=(c->phase==ResourceLifecyclePhase::Active) ||
        row->pins[size_t(ResourcePinClass::Preparation)]==0 || (!row->active && row->retiredBy!=c->serial)) return false;
    if(s.target==ResourceLifecyclePhase::Retired &&
        (row->pins[size_t(ResourcePinClass::LiveDependency)]!=releasedCount(outgoing,c->handles[i])+releasedCount(additional,c->handles[i]) ||
         (s.policy==ResourceRetirementPolicy::BlockPreparations && row->pins[size_t(ResourcePinClass::Preparation)]!=1))) return false;
  }
  return true;
}
template<class Validate>
void appendOutgoing(Catalog& owner,std::vector<OutgoingPin>& outgoing,uint64_t& charge,
    std::span<const ResourcePin* const> input,size_t rows,uint64_t stagedWork,Validate&& validate) {
  if(input.empty()) {if(!validate(std::span<const OutgoingPin>(outgoing),std::span<const OutgoingPin>{})) fail("Resource lifecycle dependency mutation is stale or blocked");return;}
  auto additional=owner.captureOutgoing(input);
  TemporaryCharge captured(owner,outgoingBytes(additional));
  owner.work(add(normalizationWork(input.size()),add(mul(outgoing.size(),additional.size()),
      mul(3,add(transitionWork(owner,outgoing,rows,stagedWork),transitionWork(owner,additional,rows))))));
  if(add(outgoing.size(),additional.size())>owner.limits.maxPinSets) limit("Resource lifecycle outgoing dependency limit exceeded");
  for(const auto& proof:additional)
    if(std::ranges::find(outgoing,proof.serial,&OutgoingPin::serial)!=outgoing.end())
      fail("Outgoing dependency owner is already attached to this mutation");
  if(!validate(std::span<const OutgoingPin>(outgoing),std::span<const OutgoingPin>(additional)))
    fail("Resource lifecycle dependency mutation is stale or blocked");
  // Reserve the complete prospective control array while the old array and
  // captured input are both charged. No old proof/pin/counter changes until all
  // fallible allocations and exact prospective validation have succeeded.
  const auto oldBytes=outgoingBytes(outgoing);
  const auto count=add(outgoing.size(),additional.size());
  uint64_t nextBytes=add(oldBytes,mul(additional.size(),sizeof(OutgoingPin)));
  for(const auto& proof:additional) nextBytes=add(nextBytes,mul(proof.handles.capacity(),sizeof(ResourceHandle)));
  const auto delta=nextBytes-oldBytes,nextCharge=add(charge,delta);
  TemporaryCharge controls(owner,mul(count,sizeof(OutgoingPin)));
  outgoing.reserve(count);
  static_assert(std::is_nothrow_move_constructible_v<OutgoingPin>);
  // Publication within preparation is a preallocated append. Proof ordering is
  // immaterial; each individual owner's exact handle rows stay sorted/unique.
  for(auto& proof:additional) outgoing.push_back(std::move(proof));
  charge=nextCharge;
  const auto covered=captured.release()+controls.release();owner.bytes-=covered-delta;
}
std::unique_ptr<Pin> makePin(const std::shared_ptr<Catalog>& owner,std::span<const ResourceHandle> input,
    ResourcePinClass kind,std::string_view reason) {
  auto& state=*owner;state.open();if(!valid(kind)) fail("Resource lifecycle pin class is invalid");state.reason(reason);
  state.work(add(normalizationWork(input.size()),reason.size()));
  if(state.pinSets>=state.limits.maxPinSets || state.pinReferences==state.limits.maxPinnedReferences)
    limit("Resource lifecycle aggregate pin limit exceeded");
  auto handles=normalize(state,input);
  if(state.pinSets>=state.limits.maxPinSets || handles.size()>state.limits.maxPinnedReferences-state.pinReferences)
    limit("Resource lifecycle aggregate pin limit exceeded");
  const auto charge=add(sizeof(Pin)+sizeof(Pin*)+64,add(reason.size()+1,mul(handles.capacity(),sizeof(ResourceHandle))));
  state.room(charge);
  for(const auto& handle:handles) {
    const auto* row=state.cell(handle);
    if(kind==ResourcePinClass::LiveDependency && !row->active) fail("A retired resource cannot acquire a live dependency");
    if(row->pins[size_t(kind)]==UINT64_MAX) limit("Resource lifecycle pin counter exhausted");
  }
  if(state.nextPin==UINT64_MAX) limit("Resource lifecycle pin authority exhausted");
  auto result=std::make_unique<Pin>();result->owner=owner;result->kind=kind;result->reason=reason;
  result->handles=std::move(handles);result->serial=state.nextPin++;
  state.pins.reserve(state.pinSets+1);
  for(const auto& handle:result->handles) ++state.cell(handle)->pins[size_t(kind)];
  ++state.pinSets;state.pinReferences+=result->handles.size();state.bytes+=charge;result->charge=charge;result->registered=true;
  state.pins.push_back(result.get());return result;
}
void retirementReady(const Catalog& state,std::span<const ResourceHandle> handles,ResourceRetirementPolicy policy,
    uint64_t ownPreparations,std::span<const OutgoingPin> outgoing={}) {
  if(!valid(policy)) fail("Resource lifecycle retirement policy is invalid");
  for(const auto& handle:handles) {
    const auto* row=state.cell(handle);
    if(!row || !row->active) fail("Retirement requires the current active resource incarnation");
    if(row->pins[size_t(ResourcePinClass::LiveDependency)]!=releasedCount(outgoing,handle))
      fail("A live dependency blocks resource retirement");
    if(policy==ResourceRetirementPolicy::BlockPreparations && row->pins[size_t(ResourcePinClass::Preparation)]>ownPreparations)
      fail("A preparation pin blocks resource retirement");
  }
}
} // namespace resource_lifecycle_detail

using namespace resource_lifecycle_detail;

ResourceLifecycleDomain::operator bool() const noexcept {return bool(state_);}
ResourceDomainKind ResourceLifecycleDomain::kind() const noexcept {return state_?state_->kind:ResourceDomainKind::Count;}
uint64_t ResourceLifecycleDomain::identity() const noexcept {return state_?state_->serial:0;}
ResourceHandle::operator bool() const noexcept {return bool(state_);}
ResourceKind ResourceHandle::kind() const noexcept {return state_?state_->kind:ResourceKind::Count;}
ResourceLifecycleDomain ResourceHandle::domain() const noexcept {
  ResourceLifecycleDomain result;if(state_) result.state_=state_->domain;return result;
}
uint32_t ResourceHandle::slot() const noexcept {return state_?state_->slot:UINT32_MAX;}
uint64_t ResourceHandle::incarnation() const noexcept {return state_?state_->incarnation:0;}
ResourcePin::ResourcePin() noexcept=default;ResourcePin::~ResourcePin()=default;
ResourcePin::ResourcePin(ResourcePin&&) noexcept=default;ResourcePin& ResourcePin::operator=(ResourcePin&&) noexcept=default;
ResourcePin::operator bool() const noexcept {return state_ && state_->registered;}
bool ResourcePin::active() const noexcept {return state_ && state_->registered && state_->phase==Pin::Phase::Active;}
bool ResourcePin::resourcesActive() const noexcept {
  if(!active() || state_->owner->closed) return false;
  for(const auto& handle:state_->handles) {const auto* row=state_->owner->cell(handle);if(!row || !row->active) return false;}
  return true;
}
bool ResourcePin::pending() const noexcept {return state_ && state_->registered && state_->phase==Pin::Phase::Pending;}
ResourcePinClass ResourcePin::pinClass() const noexcept {return state_?state_->kind:ResourcePinClass::Count;}
std::span<const ResourceHandle> ResourcePin::resources() const noexcept {return state_ && state_->registered?std::span<const ResourceHandle>(state_->handles):std::span<const ResourceHandle>{};}
void ResourcePin::reset() noexcept {state_.reset();}
ResourceRetirementCommand::operator bool() const noexcept {return state_ && state_->registered;}
bool ResourceRetirementCommand::published() const noexcept {return state_ && state_->published;}
ResourceLifecyclePhase ResourceRetirementCommand::phase() const noexcept {return state_?state_->phase:ResourceLifecyclePhase::Active;}
std::span<const ResourceHandle> ResourceRetirementCommand::resources() const noexcept {return state_?std::span<const ResourceHandle>(state_->handles):std::span<const ResourceHandle>{};}
ResourceAppendCommand::operator bool() const noexcept {return state_ && state_->registered && state_->append;}
bool ResourceAppendCommand::published() const noexcept {return state_ && state_->registered && state_->published;}
ResourceLifecyclePhase ResourceAppendCommand::phase() const noexcept {return state_?state_->phase:ResourceLifecyclePhase::Active;}
std::span<const ResourceHandle> ResourceAppendCommand::resources() const noexcept {
  return state_ && state_->registered?std::span<const ResourceHandle>(state_->handles):std::span<const ResourceHandle>{};
}
PreparedResourceTransition::PreparedResourceTransition() noexcept=default;PreparedResourceTransition::~PreparedResourceTransition()=default;
PreparedResourceTransition::PreparedResourceTransition(PreparedResourceTransition&&) noexcept=default;
PreparedResourceTransition& PreparedResourceTransition::operator=(PreparedResourceTransition&&) noexcept=default;
PreparedResourceTransition::operator bool() const noexcept {return state_ && !state_->consumed && !state_->owner->closed;}
ResourceRetirementCommand PreparedResourceTransition::command() const noexcept {return state_?state_->command:ResourceRetirementCommand{};}
uint64_t PreparedResourceTransition::expectedRevision() const noexcept {return state_?state_->revision:0;}
PreparedResourceRegistration::PreparedResourceRegistration() noexcept=default;PreparedResourceRegistration::~PreparedResourceRegistration()=default;
PreparedResourceRegistration::PreparedResourceRegistration(PreparedResourceRegistration&&) noexcept=default;
PreparedResourceRegistration& PreparedResourceRegistration::operator=(PreparedResourceRegistration&&) noexcept=default;
PreparedResourceRegistration::operator bool() const noexcept {return state_ && !state_->consumed && !state_->owner->closed;}
std::span<const ResourceHandle> PreparedResourceRegistration::resources() const noexcept {
  return state_?std::span<const ResourceHandle>(state_->handles):std::span<const ResourceHandle>{};
}
uint64_t PreparedResourceRegistration::expectedRevision() const noexcept {return state_?state_->revision:0;}
ResourceCatalogSnapshot::operator bool() const noexcept {return bool(state_);}
uint64_t ResourceCatalogSnapshot::revision() const noexcept {return state_?state_->revision:0;}
std::span<const ResourceInventoryRow> ResourceCatalogSnapshot::rows() const noexcept {return state_?std::span<const ResourceInventoryRow>(state_->rows):std::span<const ResourceInventoryRow>{};}
std::span<const ResourceInventoryReason> ResourceCatalogSnapshot::reasons() const noexcept {return state_?std::span<const ResourceInventoryReason>(state_->reasons):std::span<const ResourceInventoryReason>{};}

ResourceLifecycleCatalog::ResourceLifecycleCatalog(const ResourceLifecycleLimits& limits) {
  for(const auto value:{limits.maxDomains,limits.maxSlots,limits.maxPinSets,limits.maxPinnedReferences,limits.maxCommands,
      limits.maxCommandReferences,limits.maxSnapshots,limits.maxSnapshotRows,limits.maxRegistrations,
      limits.maxRegistrationReferences,limits.maxOwnedBytes,limits.maxWork,limits.maxReasonBytes})
    if(!value || value==UINT64_MAX) fail("Resource lifecycle limits must be positive and finite");
  if(limits.maxOwnedBytes<sizeof(Catalog) || limits.maxSlots>UINT32_MAX) fail("Resource lifecycle limits cannot represent the catalog");
  state_=std::make_shared<Catalog>();state_->limits=limits;
}
ResourceLifecycleCatalog::~ResourceLifecycleCatalog() {
  state_->closed=true;
  for(auto* registration:state_->registrationOwners) {registration->cancelPins();registration->cancelAppend();}
  for(auto* transition:state_->transitionOwners) transition->cancelPins();
}
uint64_t ResourceLifecycleCatalog::revision() const noexcept {return state_->revision;}
uint64_t ResourceLifecycleCatalog::ownedBytes() const noexcept {return state_->bytes;}
ResourceLifecycleDomain ResourceLifecycleCatalog::createDomain(ResourceDomainKind kind) {
  auto& state=*state_;state.open();if(!valid(kind)) fail("Resource lifecycle domain kind is invalid");
  if(state.domains.size()>=state.limits.maxDomains || state.nextDomain==UINT64_MAX || state.revision==UINT64_MAX)
    limit("Resource lifecycle domain or revision limit exceeded");
  state.work(1);const uint64_t charge=sizeof(Domain)+64;state.room(add(charge,sizeof(DomainRows)));
  auto value=std::make_shared<Domain>();value->owner=state.owner;value->index=state.domains.size();
  value->serial=state.nextDomain++;value->kind=kind;value->accounting=state_;
  state.domains.reserve(state.domains.size()+1);
  ResourceLifecycleDomain result;result.state_=std::move(value);
  state.domains.push_back({result,{}});state.bytes+=charge+sizeof(DomainRows);
  const_cast<Domain*>(result.state_.get())->charge=charge;++state.revision;return result;
}
PreparedResourceRegistration ResourceLifecycleCatalog::prepareIssue(std::span<const ResourceRegistrationRequest> input) {
  auto& state=*state_;state.open();if(input.empty()) fail("Resource lifecycle registration is empty");
  state.work(normalizationWork(input.size()));
  if(state.registrations>=state.limits.maxRegistrations || input.size()>state.limits.maxRegistrationReferences-state.registrationReferences)
    limit("Resource lifecycle aggregate registration limit exceeded");
  const auto scratch=mul(input.size(),sizeof(ResourceRegistrationRequest));
  const auto perRow=sizeof(ResourceHandle)+sizeof(Handle)+64+sizeof(Cell);
  state.room(add(sizeof(Registration)+sizeof(Registration*)+64,add(scratch,mul(input.size(),perRow))));
  if(input.size()>=UINT64_MAX-state.nextIncarnation || state.revision==UINT64_MAX)
    limit("Resource lifecycle incarnation or revision authority exhausted");
  for(const auto& request:input) if(!state.domain(request.domain) || !valid(request.kind))
    fail("Resource lifecycle registration domain/kind is invalid or foreign");
  TemporaryCharge requestScratch(state,scratch);
  std::vector<ResourceRegistrationRequest> requests(input.begin(),input.end());
  const auto key=[](const auto& r){return std::tuple(r.domain.identity(),r.kind,r.slot);};
  std::ranges::sort(requests,[&](const auto& a,const auto& b){return key(a)<key(b);});
  uint64_t newCells=0,nextAppend=0;
  for(size_t i=0;i<requests.size();++i) {
    const auto& r=requests[i];auto& table=state.domain(r.domain)->tables[size_t(r.kind)];
    if(i && key(r)==key(requests[i-1])) fail("Resource lifecycle registration repeats a slot");
    if(!i || r.domain!=requests[i-1].domain || r.kind!=requests[i-1].kind) nextAppend=table.size();
    if(r.slot<table.size()) {if(!Catalog::reclaimable(table[r.slot])) fail("Resource lifecycle slot is active or retained");}
    else {
      if(uint64_t(r.slot)!=nextAppend) fail("Resource lifecycle registration creates a slot hole");
      ++newCells;++nextAppend;
    }
  }
  if(newCells>state.limits.maxSlots-state.slots) limit("Resource lifecycle aggregate slot limit exceeded");
  auto prepared=std::make_unique<Registration>();prepared->owner=state_;prepared->revision=state.revision;
  state.registrationOwners.reserve(state.registrationOwners.size()+1);
  prepared->handles.reserve(requests.size());
  for(const auto& request:requests) {
    auto value=std::make_shared<Handle>();value->domain=request.domain.state_;value->kind=request.kind;value->slot=request.slot;
    value->incarnation=state.nextIncarnation++;value->accounting=state_;value->charge=sizeof(Handle)+64;state.bytes+=value->charge;
    ResourceHandle handle;handle.state_=std::move(value);prepared->handles.push_back(std::move(handle));
  }
  // Reserving invisible capacity is allowed to fail without publishing rows.
  for(size_t i=0;i<requests.size();) {
    const auto& r=requests[i];auto& table=state.domain(r.domain)->tables[size_t(r.kind)];size_t end=i+1;
    while(end<requests.size() && requests[end].domain==r.domain && requests[end].kind==r.kind) ++end;
    table.reserve(std::max<uint64_t>(table.size(),uint64_t(requests[end-1].slot)+1));i=end;
  }
  const auto charge=add(sizeof(Registration)+sizeof(Registration*)+64,add(mul(prepared->handles.capacity(),sizeof(ResourceHandle)),mul(newCells,sizeof(Cell))));
  state.room(charge);
  prepared->newCells=newCells;prepared->charge=charge;prepared->registered=true;
  state.bytes+=charge;++state.registrations;state.registrationReferences+=prepared->handles.size();
  state.registrationOwners.push_back(prepared.get());
  PreparedResourceRegistration result;result.state_=std::move(prepared);return result;
}
ResourceAppendCommand ResourceLifecycleCatalog::retainAppendAuthority(PreparedResourceRegistration& registration,std::string_view reason) {
  auto& state=*state_;state.open();state.reason(reason);
  if(!canExchange(registration) || registration.state_->append)
    fail("Append authority requires its exact original unpublished registration and can be retained only once");
  auto& prepared=*registration.state_;state.work(add(mul(prepared.handles.size(),3),reason.size()));
  if(state.commands.size()>=state.limits.maxCommands ||
      prepared.handles.size()>state.limits.maxCommandReferences-state.commandReferences || state.nextCommand==UINT64_MAX)
    limit("Resource lifecycle append authority limit exceeded");
  const auto charge=add(sizeof(Command)+sizeof(Command*)+64,add(reason.size()+1,
      mul(prepared.handles.size(),sizeof(ResourceHandle)+2*sizeof(uint64_t))));
  state.room(charge);
  auto command=std::make_shared<Command>();command->owner=state_;command->append=true;command->registration=&prepared;
  command->handles=prepared.handles;command->phaseRevisions.assign(prepared.handles.size(),1);
  command->resumeEndpoints.assign(prepared.handles.size(),1);command->reason=reason;
  state.commands.reserve(state.commands.size()+1);
  command->serial=state.nextCommand++;command->registered=true;command->charge=charge;
  state.commandReferences+=command->handles.size();state.bytes+=charge;state.commands.push_back(command.get());
  prepared.append=command;ResourceAppendCommand result;result.state_=std::move(command);return result;
}
bool ResourceLifecycleCatalog::canExchange(const PreparedResourceRegistration& prepared) const noexcept {
  if(!prepared.state_ || prepared.state_->consumed || prepared.state_->owner!=state_ || state_->closed ||
      prepared.state_->revision!=state_->revision || state_->revision==UINT64_MAX) return false;
  const auto& s=*prepared.state_;uint64_t newCells=0;uint64_t lastDomain=0;ResourceKind lastKind=ResourceKind::Count;size_t next=0;
  if(s.append && (!s.append->registered || !s.append->append || s.append->published || s.append->registration!=&s ||
      s.append->owner!=state_ || s.append->handles!=s.handles || s.append->phaseRevisions.size()!=s.handles.size() ||
      s.append->resumeEndpoints.size()!=s.handles.size())) return false;
  if(!outgoingMatches(*state_,s.outgoing)) return false;
  for(const auto& handle:s.handles) {
    const auto* rows=state_->domain(handle.domain());if(!rows || !valid(handle.kind())) return false;
    const auto& table=rows->tables[size_t(handle.kind())];
    if(lastDomain!=handle.domain().identity() || lastKind!=handle.kind()) {next=table.size();lastDomain=handle.domain().identity();lastKind=handle.kind();}
    if(handle.slot()<table.size()) {if(!Catalog::reclaimable(table[handle.slot()])) return false;}
    else {if(handle.slot()!=next || table.capacity()<=handle.slot()) return false;++next;++newCells;}
  }
  if(newCells!=s.newCells || newCells>state_->limits.maxSlots-state_->slots ||
      state_->pins.capacity()<state_->pins.size()+s.pendingPins.size()) return false;
  for(const auto* pin:s.pendingPins) {
    if(!pin || !pin->registered || pin->phase!=Pin::Phase::Pending || pin->registration!=&s ||
        pin->owner!=state_ || !valid(pin->kind)) return false;
    for(const auto& handle:pin->handles) if(!currentOrProspective(*state_,s,handle)) return false;
  }
  return true;
}
bool ResourceLifecycleCatalog::exchange(PreparedResourceRegistration& prepared) noexcept {
  if(!canExchange(prepared)) return false;auto& s=*prepared.state_;
  releaseOutgoing(*state_,s.outgoing);
  for(const auto& handle:s.handles) {
    auto& table=state_->domain(handle.domain())->tables[size_t(handle.kind())];
    if(handle.slot()==table.size()) table.push_back(Cell{handle});else table[handle.slot()]=Cell{handle};
  }
  if(s.append) {
    for(const auto& handle:s.handles) ++state_->cell(handle)->commands;
    s.append->registration=nullptr;s.append->published=true;
  }
  for(auto* pin:s.pendingPins) {
    for(const auto& handle:pin->handles) ++state_->cell(handle)->pins[size_t(pin->kind)];
    pin->phase=Pin::Phase::Active;pin->registration=nullptr;state_->pins.push_back(pin);
  }
  s.pendingPins.clear();s.stagedReferences=0;
  state_->slots+=s.newCells;s.charge-=s.newCells*sizeof(Cell);s.consumed=true;++state_->revision;return true;
}
ResourceHandle ResourceLifecycleCatalog::issue(const ResourceLifecycleDomain& domain,ResourceKind kind,uint32_t slot) {
  const ResourceRegistrationRequest request{domain,kind,slot};auto prepared=prepareIssue(std::span(&request,1));
  const auto result=prepared.resources().front();if(!exchange(prepared)) fail("Resource lifecycle registration became stale");return result;
}
bool ResourceLifecycleCatalog::contains(const ResourceHandle& handle) const noexcept {return !state_->closed && state_->cell(handle);}
bool ResourceLifecycleCatalog::active(const ResourceHandle& handle) const noexcept {const auto* row=state_->cell(handle);return !state_->closed && row && row->active;}
bool ResourceLifecycleCatalog::reclaimable(const ResourceHandle& handle) const noexcept {const auto* row=state_->cell(handle);return !state_->closed && row && Catalog::reclaimable(*row);}
ResourcePin ResourceLifecycleCatalog::pin(std::span<const ResourceHandle> input,ResourcePinClass kind,std::string_view reason) {
  ResourcePin result;result.state_=makePin(state_,input,kind,reason);return result;
}
ResourcePin ResourceLifecycleCatalog::stagePin(PreparedResourceRegistration& registration,
    std::span<const ResourceHandle> input,ResourcePinClass kind,std::string_view reason) {
  auto& state=*state_;state.open();
  if(!valid(kind)) fail("Resource lifecycle pin class is invalid");state.reason(reason);
  if(!canExchange(registration)) fail("Resource lifecycle pin registration is stale, foreign or consumed");
  if(input.empty()) fail("Resource lifecycle inventory is empty");
  auto& prepared=*registration.state_;
  const auto lookupWork=add(4,std::bit_width(prepared.handles.size()));
  // Include every previously attached set in the bound on final validation.
  state.work(add(add(normalizationWork(input.size()),reason.size()),
      transitionWork(state,prepared.outgoing,prepared.handles.size(),
          mul(add(prepared.stagedReferences,input.size()),lookupWork))));
  if(state.pinSets>=state.limits.maxPinSets || state.pinReferences==state.limits.maxPinnedReferences)
    limit("Resource lifecycle aggregate pin limit exceeded");
  state.room(mul(input.size(),sizeof(ResourceHandle)));
  for(const auto& handle:input) if(!currentOrProspective(state,prepared,handle))
    fail("Resource lifecycle pending pin has a stale, foreign or noncandidate handle");
  std::vector<ResourceHandle> handles(input.begin(),input.end());
  std::ranges::sort(handles,lessHandle);handles.erase(std::unique(handles.begin(),handles.end()),handles.end());
  if(handles.size()>state.limits.maxPinnedReferences-state.pinReferences) limit("Resource lifecycle aggregate pin limit exceeded");
  const auto charge=add(sizeof(Pin)+2*sizeof(Pin*)+64,add(reason.size()+1,mul(handles.capacity(),sizeof(ResourceHandle))));
  state.room(charge);if(state.nextPin==UINT64_MAX) limit("Resource lifecycle pin authority exhausted");
  auto pending=std::make_unique<Pin>();pending->owner=state_;pending->kind=kind;pending->reason=reason;
  pending->handles=std::move(handles);pending->serial=state.nextPin++;pending->phase=Pin::Phase::Pending;
  // Both registries have their complete eventual publication capacity before
  // any ownership/accounting is installed. Failure leaves registration usable.
  state.pins.reserve(state.pinSets+1);prepared.pendingPins.reserve(prepared.pendingPins.size()+1);
  pending->registration=&prepared;pending->charge=charge;pending->registered=true;
  ++state.pinSets;state.pinReferences+=pending->handles.size();state.bytes+=charge;
  prepared.stagedReferences+=pending->handles.size();prepared.pendingPins.push_back(pending.get());
  ResourcePin result;result.state_=std::move(pending);return result;
}
ResourcePin ResourceLifecycleCatalog::stagePin(PreparedResourceTransition& transition,
    std::span<const ResourceHandle> input,ResourcePinClass kind,std::string_view reason) {
  auto& state=*state_;state.open();
  if(!valid(kind)) fail("Resource lifecycle pin class is invalid");state.reason(reason);
  if(!canExchange(transition)) fail("Resource lifecycle pin transition is stale, foreign or consumed");
  if(input.empty()) fail("Resource lifecycle inventory is empty");
  auto& prepared=*transition.state_;const auto selected=transitionResources(prepared);
  const auto lookupWork=add(4,std::bit_width(selected.size()));
  state.work(add(add(normalizationWork(input.size()),reason.size()),
      transitionWork(state,prepared.outgoing,selected.size(),
          mul(add(prepared.stagedReferences,input.size()),lookupWork))));
  if(state.pinSets>=state.limits.maxPinSets || state.pinReferences==state.limits.maxPinnedReferences)
    limit("Resource lifecycle aggregate pin limit exceeded");
  state.room(mul(input.size(),sizeof(ResourceHandle)));
  for(const auto& handle:input) if(!transitionHandle(state,prepared,selected,handle,kind))
    fail("Resource lifecycle pending pin has a stale, foreign or noncandidate handle/phase");
  std::vector<ResourceHandle> handles(input.begin(),input.end());
  std::ranges::sort(handles,lessHandle);handles.erase(std::unique(handles.begin(),handles.end()),handles.end());
  if(handles.size()>state.limits.maxPinnedReferences-state.pinReferences) limit("Resource lifecycle aggregate pin limit exceeded");
  const auto charge=add(sizeof(Pin)+2*sizeof(Pin*)+64,add(reason.size()+1,mul(handles.capacity(),sizeof(ResourceHandle))));
  state.room(charge);if(state.nextPin==UINT64_MAX) limit("Resource lifecycle pin authority exhausted");
  auto pending=std::make_unique<Pin>();pending->owner=state_;pending->kind=kind;pending->reason=reason;
  pending->handles=std::move(handles);pending->serial=state.nextPin++;pending->phase=Pin::Phase::Pending;
  state.pins.reserve(state.pinSets+1);prepared.pendingPins.reserve(prepared.pendingPins.size()+1);
  pending->transition=&prepared;pending->charge=charge;pending->registered=true;
  ++state.pinSets;state.pinReferences+=pending->handles.size();state.bytes+=charge;
  prepared.stagedReferences+=pending->handles.size();prepared.pendingPins.push_back(pending.get());
  ResourcePin result;result.state_=std::move(pending);return result;
}
void ResourceLifecycleCatalog::stageDependencyRelease(PreparedResourceRegistration& registration,
    std::span<const ResourcePin* const> input) {
  auto& state=*state_;state.open();
  if(!canExchange(registration)) fail("Resource lifecycle dependency registration is stale, foreign or consumed");
  auto& prepared=*registration.state_;
  const auto lookupWork=add(4,std::bit_width(prepared.handles.size()));
  appendOutgoing(state,prepared.outgoing,prepared.charge,input,prepared.handles.size(),
      mul(prepared.stagedReferences,lookupWork),[&](auto,auto additional) noexcept {
        return canExchange(registration) && outgoingMatches(state,additional);
      });
}
void ResourceLifecycleCatalog::stageDependencyRelease(PreparedResourceTransition& transition,
    std::span<const ResourcePin* const> input) {
  auto& state=*state_;state.open();
  if(!transition.state_ || transition.state_->consumed || transition.state_->owner!=state_ ||
      transition.state_->revision!=state.revision)
    fail("Resource lifecycle dependency transition is stale, foreign or consumed");
  auto& prepared=*transition.state_;
  const auto* command=prepared.command.state_.get();
  const auto rows=transitionResources(prepared).size(),lookupWork=add(4,std::bit_width(rows));
  appendOutgoing(state,prepared.outgoing,prepared.charge,input,rows,mul(prepared.stagedReferences,lookupWork),
      [&](auto old,auto additional) noexcept {return transitionReady(state,prepared,command,old,additional);});
}
PreparedResourceTransition ResourceLifecycleCatalog::prepareRetirement(std::span<const ResourceHandle> input,
    std::string_view reason,ResourceRetirementPolicy policy) {
  return prepareRetirement(input,{},reason,policy);
}
PreparedResourceTransition ResourceLifecycleCatalog::prepareRetirement(std::span<const ResourceHandle> input,
    std::span<const ResourcePin* const> outgoingLivePins,std::string_view reason,ResourceRetirementPolicy policy) {
  auto& state=*state_;state.open();state.reason(reason);
  state.work(add(mul(2,normalizationWork(input.size())),add(reason.size(),64)));
  if(state.commands.size()>=state.limits.maxCommands || state.commandReferences==state.limits.maxCommandReferences)
    limit("Resource lifecycle aggregate command limit exceeded");
  auto outgoing=state.captureOutgoing(outgoingLivePins);
  const auto transitionBytes=add(sizeof(Transition)+sizeof(Transition*)+64,outgoingBytes(outgoing));
  TemporaryCharge transitionCharge(state,transitionBytes);
  auto handles=normalize(state,input);
  state.work(add(add(mul(2,normalizationWork(input.size())),add(reason.size(),64)),
      transitionWork(state,outgoing,handles.size())));
  retirementReady(state,handles,policy,0,outgoing);
  if(state.commands.size()>=state.limits.maxCommands || handles.size()>state.limits.maxCommandReferences-state.commandReferences)
    limit("Resource lifecycle aggregate command limit exceeded");
  if(state.nextCommand==UINT64_MAX || state.revision==UINT64_MAX) limit("Resource lifecycle retirement authority exhausted");
  const auto charge=add(sizeof(Command)+sizeof(Command*)+64,add(reason.size()+1,
      add(mul(handles.capacity(),sizeof(ResourceHandle)),mul(handles.size(),sizeof(uint64_t)))));
  state.room(charge);
  auto command=std::make_shared<Command>();command->owner=state_;command->handles=std::move(handles);command->reason=reason;
  command->phaseRevisions.reserve(command->handles.size());
  for(const auto& handle:command->handles) {
    const auto* row=state.cell(handle);if(row->commands==UINT64_MAX || row->phaseRevision==UINT64_MAX) limit("Resource lifecycle row authority exhausted");
    command->phaseRevisions.push_back(row->endpointRevision);
  }
  state.commands.reserve(state.commands.size()+1);command->serial=state.nextCommand++;
  for(const auto& handle:command->handles) ++state.cell(handle)->commands;
  command->charge=charge;command->registered=true;state.bytes+=charge;state.commandReferences+=command->handles.size();state.commands.push_back(command.get());
  ResourceRetirementCommand authority;authority.state_=std::move(command);
  auto prepared=std::make_unique<Transition>();prepared->owner=state_;prepared->command=authority;
  prepared->outgoing=std::move(outgoing);
  state.transitionOwners.reserve(state.transitionOwners.size()+1);
  prepared->preparation=pin(authority.resources(),ResourcePinClass::Preparation,"Prepared resource retirement");
  prepared->charge=transitionCharge.release();
  prepared->revision=state.revision;prepared->target=ResourceLifecyclePhase::Retired;prepared->policy=policy;
  prepared->registered=true;state.transitionOwners.push_back(prepared.get());
  PreparedResourceTransition result;result.state_=std::move(prepared);return result;
}
PreparedResourceTransition ResourceLifecycleCatalog::prepareChange(const ResourceRetirementCommand& command,
    ResourceLifecyclePhase target,ResourceRetirementPolicy policy) {
  return prepareChange(command,target,{},policy);
}
PreparedResourceTransition ResourceLifecycleCatalog::prepareChange(const ResourceRetirementCommand& command,
    ResourceLifecyclePhase target,std::span<const ResourcePin* const> outgoingLivePins,ResourceRetirementPolicy policy) {
  auto& state=*state_;state.open();if(!valid(target) || !valid(policy) || !command.state_ ||
      command.state_->owner!=state_ || command.state_->append || !command.state_->registered || !command.state_->published || command.state_->phase==target)
    fail("Resource lifecycle retirement command/phase is invalid, foreign or unpublished");
  auto outgoing=state.captureOutgoing(outgoingLivePins);
  state.work(add(add(normalizationWork(command.resources().size()),add(command.resources().size(),64)),
      transitionWork(state,outgoing,command.resources().size())));
  TemporaryCharge transitionCharge(state,add(sizeof(Transition)+sizeof(Transition*)+64,outgoingBytes(outgoing)));
  const auto& c=*command.state_;
  for(size_t i=0;i<c.handles.size();++i) {
    const auto* row=state.cell(c.handles[i]);
    if(!row || row->endpointRevision!=c.phaseRevisions[i] || row->active!=(c.phase==ResourceLifecyclePhase::Active) ||
        (!row->active && row->retiredBy!=c.serial)) fail("Resource lifecycle command no longer owns the selected phase");
    if(row->phaseRevision==UINT64_MAX) limit("Resource lifecycle row authority exhausted");
  }
  if(target==ResourceLifecyclePhase::Retired) retirementReady(state,c.handles,policy,0,outgoing);
  if(state.revision==UINT64_MAX || c.version==UINT64_MAX) limit("Resource lifecycle transition authority exhausted");
  auto prepared=std::make_unique<Transition>();prepared->owner=state_;prepared->command=command;
  prepared->outgoing=std::move(outgoing);
  state.transitionOwners.reserve(state.transitionOwners.size()+1);
  prepared->preparation=pin(command.resources(),ResourcePinClass::Preparation,"Prepared resource history replay");
  prepared->charge=transitionCharge.release();
  prepared->revision=state.revision;prepared->commandVersion=c.version;prepared->target=target;prepared->policy=policy;
  prepared->registered=true;state.transitionOwners.push_back(prepared.get());
  PreparedResourceTransition result;result.state_=std::move(prepared);return result;
}
PreparedResourceTransition ResourceLifecycleCatalog::prepareDependencyChange(
    std::span<const ResourcePin* const> outgoingLivePins,std::string_view reason) {
  auto& state=*state_;state.open();state.reason(reason);
  auto outgoing=state.captureOutgoing(outgoingLivePins);
  state.work(add(add(transitionWork(state,outgoing,0),reason.size()),64));
  if(state.revision==UINT64_MAX) limit("Resource lifecycle transition authority exhausted");
  TemporaryCharge transitionCharge(state,add(sizeof(Transition)+sizeof(Transition*)+64,outgoingBytes(outgoing)));
  auto prepared=std::make_unique<Transition>();prepared->owner=state_;prepared->outgoing=std::move(outgoing);
  state.transitionOwners.reserve(state.transitionOwners.size()+1);
  prepared->charge=transitionCharge.release();prepared->revision=state.revision;
  prepared->registered=true;state.transitionOwners.push_back(prepared.get());
  PreparedResourceTransition result;result.state_=std::move(prepared);return result;
}
PreparedResourceTransition ResourceLifecycleCatalog::prepareAppendChange(const ResourceAppendCommand& command,
    ResourceLifecyclePhase target,std::span<const ResourcePin* const> outgoingLivePins,ResourceRetirementPolicy policy) {
  auto& state=*state_;state.open();if(!valid(target) || !valid(policy) || !command.state_ ||
      command.state_->owner!=state_ || !command.state_->registered || !command.state_->append ||
      !command.state_->published || command.state_->registration || command.state_->phase==target)
    fail("Resource lifecycle append command/phase is invalid, foreign, canceled or unpublished");
  auto outgoing=state.captureOutgoing(outgoingLivePins);const auto& append=*command.state_;
  state.work(add(add(normalizationWork(append.handles.size()),64),transitionWork(state,outgoing,append.handles.size())));
  TemporaryCharge transitionCharge(state,add(sizeof(Transition)+sizeof(Transition*)+64,
      add(outgoingBytes(outgoing),mul(append.handles.size(),2*sizeof(uint64_t)))));
  for(size_t i=0;i<append.handles.size();++i) {
    const auto* row=state.cell(append.handles[i]);
    if(!row || row->active!=(append.phase==ResourceLifecyclePhase::Active) || (!row->active &&
        (row->retiredBy!=append.serial || row->phaseRevision!=append.phaseRevisions[i])))
      fail("Original append no longer owns its complete current endpoint");
    if(row->phaseRevision==UINT64_MAX) limit("Resource lifecycle row authority exhausted");
  }
  if(target==ResourceLifecyclePhase::Retired) retirementReady(state,append.handles,policy,0,outgoing);
  if(state.revision==UINT64_MAX || append.version==UINT64_MAX) limit("Resource lifecycle append transition authority exhausted");
  auto prepared=std::make_unique<Transition>();prepared->owner=state_;prepared->append=command.state_;
  prepared->outgoing=std::move(outgoing);prepared->appendPhases.reserve(append.handles.size());prepared->appendEndpoints.reserve(append.handles.size());
  for(const auto& handle:append.handles) {
    const auto& row=*state.cell(handle);prepared->appendPhases.push_back(row.phaseRevision);prepared->appendEndpoints.push_back(row.endpointRevision);
  }
  state.transitionOwners.reserve(state.transitionOwners.size()+1);
  prepared->preparation=pin(append.handles,ResourcePinClass::Preparation,"Prepared original append suspension/replay");
  prepared->charge=transitionCharge.release();prepared->revision=state.revision;prepared->commandVersion=append.version;
  prepared->target=target;prepared->policy=policy;prepared->registered=true;state.transitionOwners.push_back(prepared.get());
  PreparedResourceTransition result;result.state_=std::move(prepared);return result;
}
bool ResourceLifecycleCatalog::canExchange(const PreparedResourceTransition& prepared) const noexcept {
  if(!prepared.state_) return false;const auto& s=*prepared.state_;
  return transitionReady(*state_,s,s.command.state_.get(),s.outgoing);
}
bool ResourceLifecycleCatalog::exchange(PreparedResourceTransition& prepared) noexcept {
  if(!canExchange(prepared)) return false;auto& s=*prepared.state_;
  releaseOutgoing(*state_,s.outgoing);
  if(s.command.state_) {
    auto& c=*s.command.state_;
    for(size_t i=0;i<c.handles.size();++i) {
      auto& row=*state_->cell(c.handles[i]);row.active=s.target==ResourceLifecyclePhase::Active;
      row.retiredBy=row.active?0:c.serial;c.phaseRevisions[i]=row.endpointRevision=++row.phaseRevision;
    }
    c.phase=s.target;c.published=true;++c.version;
  }
  if(s.append) {
    auto& append=*s.append;
    for(size_t i=0;i<append.handles.size();++i) {
      auto& row=*state_->cell(append.handles[i]);row.active=s.target==ResourceLifecyclePhase::Active;
      row.retiredBy=row.active?0:append.serial;
      if(!row.active) append.resumeEndpoints[i]=s.appendEndpoints[i];
      append.phaseRevisions[i]=++row.phaseRevision;
      row.endpointRevision=row.active?append.resumeEndpoints[i]:row.phaseRevision;
    }
    append.phase=s.target;++append.version;
  }
  // Restored phases must be active before their preallocated dependency pins
  // become visible. No token/storage allocation or payload release occurs.
  for(auto* pin:s.pendingPins) {
    for(const auto& handle:pin->handles) ++state_->cell(handle)->pins[size_t(pin->kind)];
    pin->phase=Pin::Phase::Active;pin->transition=nullptr;state_->pins.push_back(pin);
  }
  s.pendingPins.clear();s.stagedReferences=0;
  ++state_->revision;s.consumed=true;s.preparation.reset();return true;
}
ResourceCatalogSnapshot ResourceLifecycleCatalog::inventory() const {
  auto& state=*state_;state.open();
  uint64_t references=state.commandReferences;
  for(const auto* pin:state.pins) references=add(references,pin->handles.size());
  if(state.snapshots>=state.limits.maxSnapshots || state.slots>state.limits.maxSnapshotRows-state.snapshotRows)
    limit("Resource lifecycle aggregate snapshot limit exceeded");
  uint64_t charge=add(sizeof(Snapshot)+64,add(mul(state.slots,sizeof(ResourceInventoryRow)),
      mul(state.pins.size()+state.commands.size(),sizeof(ResourceInventoryReason))));
  charge=add(charge,mul(references,sizeof(ResourceHandle)));
  for(const auto* pin:state.pins) charge=add(charge,pin->reason.size()+1);
  for(const auto* command:state.commands) charge=add(charge,command->reason.size()+1);
  // Charging the complete logical output is also a conservative copy-work
  // bound, including the strings in class-aware reasons.
  state.work(charge);
  state.room(charge);
  auto snapshot=std::make_shared<Snapshot>();snapshot->owner=state_;snapshot->revision=state.revision;
  snapshot->rows.reserve(state.slots);snapshot->reasons.reserve(state.pins.size()+state.commands.size());
  for(const auto& domain:state.domains) for(const auto& table:domain.tables) for(const auto& row:table)
    snapshot->rows.push_back({row.handle,row.active,!row.active,row.pins,row.commands});
  for(const auto* pin:state.pins) snapshot->reasons.push_back({pin->kind,pin->reason,false,pin->serial,pin->handles});
  for(const auto* command:state.commands) if(!command->append || command->published)
    snapshot->reasons.push_back({ResourcePinClass::History,command->reason,true,command->serial,command->handles});
  snapshot->charge=charge;snapshot->registered=true;state.bytes+=charge;++state.snapshots;state.snapshotRows+=snapshot->rows.size();
  ResourceCatalogSnapshot result;result.state_=std::move(snapshot);return result;
}
ResourceCatalogSnapshot ResourceLifecycleCatalog::retainSnapshot(ResourcePinClass kind,std::string_view reason) {
  // The temporary selection and the eventual immutable output both count at
  // the peak. A failed snapshot releases its newly acquired pin automatically.
  auto& state=*state_;state.open();state.work(state.slots);state.room(mul(state.slots,sizeof(ResourceHandle)));
  if(!valid(kind)) fail("Resource lifecycle snapshot pin class is invalid");state.reason(reason);
  TemporaryCharge selectionCharge(state,mul(state.slots,sizeof(ResourceHandle)));
  std::vector<ResourceHandle> handles;handles.reserve(state.slots);
  for(const auto& domain:state.domains) for(const auto& table:domain.tables) for(const auto& row:table) handles.push_back(row.handle);
  ResourcePin retention;if(!handles.empty()) retention=pin(handles,kind,reason);
  ResourceCatalogSnapshot result=inventory();
  const_cast<Snapshot*>(result.state_.get())->retention=std::move(retention);return result;
}

} // namespace container::util
