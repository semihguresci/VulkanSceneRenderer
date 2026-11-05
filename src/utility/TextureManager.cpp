#include <Container/utility/TextureManager.h>
#include <Container/utility/TextureResource.h>

#include <algorithm>
#include <bit>
#include <limits>
#include <stdexcept>


namespace container::material {

uint32_t TextureManager::registerTexture(const TextureResource& resource) {
  if (!resource.name.empty()) {
    if (const auto existing = textureNameToIndex_.find(resource.name);
        existing != textureNameToIndex_.end()) {
      if(isTextureActive(existing->second)) return existing->second;
      textureNameToIndex_.erase(existing);
    }
  }

  if(textures_.size()>=std::numeric_limits<uint32_t>::max() || revision_==UINT64_MAX)
    throw std::length_error("Texture slots or mutation revision exceed their identity range");

  auto incarnation=std::make_shared<const uint8_t>(0);
  if(!ownerIdentity_) ownerIdentity_=std::make_shared<const uint8_t>(0);
  textures_.push_back(resource);
  const auto index = static_cast<uint32_t>(textures_.size() - 1);
  const auto& added=textures_[index];
  try {
    active_.push_back(1);
    identities_.push_back({std::move(incarnation),{},0});
    if (!added.name.empty()) textureNameToIndex_.emplace(added.name,index);
    if (!added.contentKey.empty()) imageContentToIndex_.try_emplace(added.contentKey,index);
  } catch(...) {
    std::erase_if(textureNameToIndex_,[&](const auto& entry){return entry.second==index;});
    std::erase_if(imageContentToIndex_,[&](const auto& entry){return entry.second==index;});
    if(identities_.size()>index) identities_.pop_back();
    if(active_.size()>index) active_.pop_back();textures_.pop_back();throw;
  }
  ++activeCount_;
  ++revision_;
  tailVacancy_=identities_[index].incarnation;
  return index;
}

std::optional<uint32_t> TextureManager::findTextureIndex(
    std::string_view name) const {
  if (name.empty()) return std::nullopt;
  if (const auto it = textureNameToIndex_.find(name);
      it != textureNameToIndex_.end()) {
    if(isTextureActive(it->second)) return it->second;
  }
  return std::nullopt;
}

const TextureResource* TextureManager::getTexture(uint32_t index) const {
  if (!isTextureActive(index)) return nullptr;
  return &textures_[index];
}

const TextureResource* TextureManager::findTextureImage(std::string_view contentKey) const {
  const auto found = imageContentToIndex_.find(contentKey);
  return found == imageContentToIndex_.end() ? nullptr : getTexture(found->second);
}

bool TextureManager::isTextureActive(uint32_t index) const noexcept {
  return index<textures_.size() && index<active_.size() && active_[index]!=0;
}

bool TextureManager::retireTexture(uint32_t index) noexcept {
  if(!isTextureActive(index) || index>=identities_.size() || revision_==UINT64_MAX || identities_[index].phaseVersion==UINT64_MAX) return false;
  std::erase_if(textureNameToIndex_,[&](const auto& entry){return entry.second==index;});
  std::erase_if(imageContentToIndex_,[&](const auto& entry){return entry.second==index;});
  textures_[index]=TextureResource{};active_[index]=0;--activeCount_;++revision_;
  ++identities_[index].phaseVersion;identities_[index].retiredBy.reset();
  return true;
}

const TextureResource& TextureManager::getTextureOrFallback(uint32_t index,const TextureResource& fallback) const noexcept {
  const auto* texture=getTexture(index);return texture?*texture:fallback;
}

TextureManager TextureManager::copyPrefix(size_t count) const {
  if(count>textures_.size() || active_.size()!=textures_.size() || identities_.size()!=textures_.size())
    throw std::invalid_argument("Texture prefix is outside its stable slot range");
  TextureManager result;
  result.textures_.assign(textures_.begin(),textures_.begin()+count);
  result.active_.assign(active_.begin(),active_.begin()+count);
  result.identities_.assign(identities_.begin(),identities_.begin()+count);result.ownerIdentity_=ownerIdentity_;
  result.tailVacancy_=count<textures_.size()?std::make_shared<const uint8_t>(0):tailVacancy_;
  result.activeCount_=static_cast<size_t>(std::count_if(result.active_.begin(),result.active_.end(),[](uint8_t active){return active!=0;}));
  result.revision_=revision_;
  for(const auto& [name,index]:textureNameToIndex_)
    if(index<count && isTextureActive(index)) result.textureNameToIndex_.emplace(name,index);
  for(const auto& [content,index]:imageContentToIndex_)
    if(index<count && isTextureActive(index)) result.imageContentToIndex_.emplace(content,index);
  return result;
}

namespace {
[[noreturn]] void runtimeInvalid(const char* text) {throw std::invalid_argument(text);}
[[noreturn]] void runtimeLimit() {throw std::length_error("Runtime texture retirement exceeds its finite budget");}
struct RuntimeBudget {
  RuntimeTextureRetirementLimits limits;
  uint64_t bytes{},work{};
  explicit RuntimeBudget(RuntimeTextureRetirementLimits value):limits(value) {
    for(const auto limit:{value.maxRows,value.maxAliases,value.maxOwnedBytes,value.maxWork})
      if(!limit || limit==UINT64_MAX || limit>SIZE_MAX) runtimeLimit();
    if(value.maxRows>UINT32_MAX) runtimeLimit();
  }
  void add(uint64_t count,uint64_t width=1) {
    if(width && count>(limits.maxOwnedBytes-bytes)/width) runtimeLimit();bytes+=count*width;
  }
  void visit(uint64_t count=1) {if(count>limits.maxWork-work) runtimeLimit();work+=count;}
  void text(const std::string& value) {add(value.size()+1);visit(value.size());}
};
struct RuntimeCheck {
  uint64_t remaining;
  bool take(uint64_t count=1) noexcept {if(count>remaining) return false;remaining-=count;return true;}
};
bool sameResource(const TextureResource& a,const TextureResource& b,RuntimeCheck& check) noexcept {
  return check.take() && a.image==b.image && a.imageView==b.imageView && a.sampler==b.sampler &&
      a.samplerIndex==b.samplerIndex && a.format==b.format && a.width==b.width && a.height==b.height &&
      a.mipLevels==b.mipLevels && a.allocationIdentity==b.allocationIdentity &&
      a.name.size()==b.name.size() && check.take(a.name.size()) && a.name==b.name &&
      a.contentKey.size()==b.contentKey.size() && check.take(a.contentKey.size()) && a.contentKey==b.contentKey;
}
struct RuntimeAlias {bool content{};std::string key;uint32_t slot{};};
struct RuntimeSavedRow {
  TextureResource resource;
  std::shared_ptr<const uint8_t> incarnation;
  uint64_t phaseVersion{};
};
struct RuntimeSelectedRows {
  std::shared_ptr<const uint8_t> ownerIdentity,identity;
  std::vector<uint32_t> slots;
  std::vector<RuntimeSavedRow> rows;
  std::vector<RuntimeAlias> aliases;
};
}
struct RuntimeTextureRetirementCommand::State : RuntimeSelectedRows {
  uint64_t bytes{},version{};
  bool published{},retired{};
};
struct RuntimeTextureAppendCommand::State : RuntimeSelectedRows {
  std::shared_ptr<const uint8_t> boundary,vacancy,resumeVacancy;
  uint32_t first{};
  uint64_t bytes{},version{};
  bool published{},present{};
};
struct PreparedRuntimeTextureAppend::State {
  const TextureManager* owner{};
  TextureManager before,candidate;
  RuntimeTextureAppendCommand receipt;
  RuntimeTextureRetirementLimits limits;
  std::vector<uint64_t> versions;
  uint64_t commandVersion{},bytes{};
  bool initial{},present{},consumed{};
};
struct PreparedRuntimeTextureRetirement::State {
  const TextureManager* owner{};
  TextureManager candidate;
  RuntimeTextureRetirementCommand receipt;
  RuntimeTextureRetirementLimits limits;
  uint64_t revision{},commandVersion{},bytes{};
  bool retiring{},initial{},consumed{};
};
struct RuntimeTextureRetirementAccess {
  using Command=RuntimeTextureRetirementCommand::State;
  using Prepared=PreparedRuntimeTextureRetirement::State;
  using Append=RuntimeTextureAppendCommand::State;
  using AppendPrepared=PreparedRuntimeTextureAppend::State;
  static auto& get(const RuntimeTextureRetirementCommand& value) noexcept {return value.state_;}
  static auto& get(const PreparedRuntimeTextureRetirement& value) noexcept {return value.state_;}
  static auto& get(const RuntimeTextureAppendCommand& value) noexcept {return value.state_;}
  static auto& get(const PreparedRuntimeTextureAppend& value) noexcept {return value.state_;}
  static auto& rows(auto& table) noexcept {return table.textures_;}
  static auto& active(auto& table) noexcept {return table.active_;}
  static auto& identities(auto& table) noexcept {return table.identities_;}
  static auto& owner(auto& table) noexcept {return table.ownerIdentity_;}
  static auto& vacancy(auto& table) noexcept {return table.tailVacancy_;}
  static auto& count(auto& table) noexcept {return table.activeCount_;}
  static auto& revision(auto& table) noexcept {return table.revision_;}
  static auto& names(auto& table) noexcept {return table.textureNameToIndex_;}
  static auto& contents(auto& table) noexcept {return table.imageContentToIndex_;}
  static RuntimeTextureRetirementCommand command(std::shared_ptr<Command> state) {
    RuntimeTextureRetirementCommand result;result.state_=std::move(state);return result;
  }
  static PreparedRuntimeTextureRetirement prepared(std::unique_ptr<Prepared> state) {
    PreparedRuntimeTextureRetirement result;result.state_=std::move(state);return result;
  }
  static RuntimeTextureAppendCommand append(std::shared_ptr<Append> state) {
    RuntimeTextureAppendCommand result;result.state_=std::move(state);return result;
  }
  static PreparedRuntimeTextureAppend appendPrepared(std::unique_ptr<AppendPrepared> state) {
    PreparedRuntimeTextureAppend result;result.state_=std::move(state);return result;
  }
};
namespace {
using RuntimeAccess=RuntimeTextureRetirementAccess;
size_t selectedRow(const RuntimeSelectedRows& command,uint32_t slot) noexcept {
  const auto found=std::lower_bound(command.slots.begin(),command.slots.end(),slot);
  return found!=command.slots.end() && *found==slot?size_t(found-command.slots.begin()):command.slots.size();
}
const RuntimeAlias* selectedAlias(const RuntimeSelectedRows& command,bool content,const std::string& key) noexcept {
  const auto found=std::lower_bound(command.aliases.begin(),command.aliases.end(),std::pair{content,std::string_view(key)},
      [](const RuntimeAlias& alias,const auto& target) {return alias.content!=target.first?alias.content<target.first:alias.key<target.second;});
  return found!=command.aliases.end() && found->content==content && found->key==key?&*found:nullptr;
}
bool cacheEndpoint(const TextureManager& table,const RuntimeSelectedRows& command,bool active,RuntimeCheck& check) noexcept {
  for(const auto* cache:{&RuntimeAccess::names(table),&RuntimeAccess::contents(table)}) {
    const bool content=cache==&RuntimeAccess::contents(table);
    for(const auto& [key,slot]:*cache) {
      if(!check.take(1+key.size())) return false;
      if(selectedRow(command,slot)==command.slots.size()) continue;
      const auto alias=selectedAlias(command,content,key);
      if(!active || !alias || alias->slot!=slot) return false;
    }
  }
  for(const auto& alias:command.aliases) {
    if(!check.take(1+alias.key.size())) return false;
    const auto& cache=alias.content?RuntimeAccess::contents(table):RuntimeAccess::names(table);const auto found=cache.find(alias.key);
    if(active?(found==cache.end() || found->second!=alias.slot):(found!=cache.end())) return false;
  }
  return true;
}
bool runtimeEndpoint(const TextureManager& table,const RuntimeAccess::Command& command,bool active,RuntimeCheck& check) noexcept {
  if(RuntimeAccess::owner(table)!=command.ownerIdentity || RuntimeAccess::rows(table).size()!=RuntimeAccess::active(table).size() ||
      RuntimeAccess::rows(table).size()!=RuntimeAccess::identities(table).size()) return false;
  const TextureResource empty;
  for(size_t i=0;i<command.slots.size();++i) {
    if(!check.take()) return false;const auto slot=command.slots[i];
    if(slot>=table.textureCount()) return false;const auto& identity=RuntimeAccess::identities(table)[slot];
    if(table.isTextureActive(slot)!=active || identity.incarnation!=command.rows[i].incarnation ||
        identity.phaseVersion!=command.rows[i].phaseVersion ||
        (active?bool(identity.retiredBy):identity.retiredBy!=command.identity) ||
        !sameResource(RuntimeAccess::rows(table)[slot],active?command.rows[i].resource:empty,check)) return false;
  }
  return cacheEndpoint(table,command,active,check);
}
void tableCost(const TextureManager& table,RuntimeBudget& budget) {
  if(table.textureCount()>budget.limits.maxRows || RuntimeAccess::rows(table).size()!=RuntimeAccess::active(table).size() ||
      RuntimeAccess::rows(table).size()!=RuntimeAccess::identities(table).size() ||
      RuntimeAccess::names(table).size()>budget.limits.maxAliases ||
      RuntimeAccess::contents(table).size()>budget.limits.maxAliases-RuntimeAccess::names(table).size()) runtimeLimit();
  budget.add(sizeof(TextureManager)+128);budget.add(table.textureCount(),TextureManager::rowStorageBytes()+64);
  uint64_t activeCount{};
  if(table.textureCount() && !RuntimeAccess::owner(table)) runtimeInvalid("Runtime table has no incarnation authority");
  for(size_t slot=0;slot<table.textureCount();++slot) {
    const auto& row=RuntimeAccess::rows(table)[slot];const auto& identity=RuntimeAccess::identities(table)[slot];
    if(RuntimeAccess::active(table)[slot]>1 || !identity.incarnation || (table.isTextureActive(uint32_t(slot)) && identity.retiredBy))
      runtimeInvalid("Runtime row phase or incarnation authority is invalid");
    activeCount+=table.isTextureActive(uint32_t(slot));budget.visit();budget.text(row.name);budget.text(row.contentKey);
  }
  if(activeCount!=table.activeTextureCount()) runtimeInvalid("Runtime active row count is inconsistent");
  for(const auto* cache:{&RuntimeAccess::names(table),&RuntimeAccess::contents(table)}) for(const auto& [key,slot]:*cache) {
    budget.add(sizeof(RuntimeAlias)+128);budget.text(key);budget.visit();
    if(!table.isTextureActive(slot)) runtimeInvalid("Runtime alias targets an inactive or missing row");
  }
}
PreparedRuntimeTextureRetirement prepareRuntime(const TextureManager& table,std::span<const uint32_t> slots,
    const RuntimeTextureRetirementCommand& receipt,RuntimeTextureRetirementTarget target,const RuntimeTextureRetirementLimits& limits) {
  RuntimeBudget budget(limits);budget.add(sizeof(RuntimeAccess::Prepared)+128);
  if(target!=RuntimeTextureRetirementTarget::Active && target!=RuntimeTextureRetirementTarget::Retired)
    runtimeInvalid("Runtime texture retirement target is invalid");
  const bool initial=!receipt,retiring=target==RuntimeTextureRetirementTarget::Retired;
  if(table.revision()==UINT64_MAX) runtimeLimit();tableCost(table,budget);
  std::shared_ptr<RuntimeAccess::Command> command;
  if(initial) {
    if(!retiring || slots.empty() || slots.size()>limits.maxRows) runtimeInvalid("Runtime retirement requires a finite nonempty selection");
    budget.add(sizeof(RuntimeAccess::Command)+256);budget.add(slots.size(),sizeof(uint32_t)+sizeof(RuntimeSavedRow));
    budget.visit(slots.size()*(4+std::bit_width(slots.size())));
    command=std::make_shared<RuntimeAccess::Command>();command->ownerIdentity=RuntimeAccess::owner(table);command->identity=std::make_shared<const uint8_t>(0);
    command->slots.assign(slots.begin(),slots.end());std::ranges::sort(command->slots);
    if(std::adjacent_find(command->slots.begin(),command->slots.end())!=command->slots.end()) runtimeInvalid("Runtime retirement contains duplicate slots");
    command->rows.reserve(command->slots.size());
    for(const auto slot:command->slots) {
      if(!table.isTextureActive(slot)) runtimeInvalid("Runtime retirement targets an inactive or missing slot");
      const auto& row=*table.getTexture(slot);const auto& identity=RuntimeAccess::identities(table)[slot];
      if(!identity.incarnation || identity.retiredBy || identity.phaseVersion==UINT64_MAX) runtimeInvalid("Runtime row has stale or exhausted phase authority");
      budget.text(row.name);budget.text(row.contentKey);command->rows.push_back({row,identity.incarnation,identity.phaseVersion});
    }
    for(const auto* cache:{&RuntimeAccess::names(table),&RuntimeAccess::contents(table)}) for(const auto& [key,slot]:*cache)
      if(selectedRow(*command,slot)!=command->slots.size()) {
        budget.add(sizeof(RuntimeAlias)+128);budget.text(key);command->aliases.push_back({cache==&RuntimeAccess::contents(table),key,slot});
      }
    budget.visit(command->aliases.size()*(4+std::bit_width(command->aliases.size())));
    std::ranges::sort(command->aliases,[](const auto& a,const auto& b) {return a.content!=b.content?a.content<b.content:a.key<b.key;});
    command->bytes=sizeof(RuntimeAccess::Command)+256+command->slots.capacity()*sizeof(uint32_t)+command->rows.capacity()*sizeof(RuntimeSavedRow)+command->aliases.capacity()*sizeof(RuntimeAlias);
    for(const auto& row:command->rows) command->bytes+=row.resource.name.size()+row.resource.contentKey.size()+2;
    for(const auto& alias:command->aliases) command->bytes+=alias.key.size()+1;
  } else {
    command=RuntimeAccess::get(receipt);budget.add(command->bytes);
    RuntimeCheck check{limits.maxWork};
    if(!command->published || command->retired==retiring || command->version==UINT64_MAX || !runtimeEndpoint(table,*command,!command->retired,check))
      runtimeInvalid("Runtime texture replay lacks its original exact phase/payload/alias authority");
    budget.visit(limits.maxWork-check.remaining);
  }
  if(!retiring) {
    // The current tombstones have no strings/cache cells. Charge the restored
    // candidate's copies separately from the command's selected originals.
    for(const auto& row:command->rows) {budget.text(row.resource.name);budget.text(row.resource.contentKey);}
    for(const auto& alias:command->aliases) {budget.add(sizeof(RuntimeAlias)+128);budget.text(alias.key);}
  }
  auto staged=std::make_unique<RuntimeAccess::Prepared>();staged->owner=&table;staged->limits=limits;staged->revision=table.revision();
  staged->commandVersion=command->version;staged->retiring=retiring;staged->initial=initial;staged->receipt=RuntimeAccess::command(command);
  staged->candidate=table;auto& candidate=staged->candidate;
  for(size_t i=0;i<command->slots.size();++i) {
    const auto slot=command->slots[i];auto& identity=RuntimeAccess::identities(candidate)[slot];
    if(identity.phaseVersion==UINT64_MAX) runtimeLimit();++identity.phaseVersion;
    RuntimeAccess::active(candidate)[slot]=retiring?0:1;identity.retiredBy=retiring?command->identity:std::shared_ptr<const uint8_t>{};
    RuntimeAccess::rows(candidate)[slot]=retiring?TextureResource{}:command->rows[i].resource;
  }
  if(retiring) {
    for(auto* cache:{&RuntimeAccess::names(candidate),&RuntimeAccess::contents(candidate)})
      std::erase_if(*cache,[&](const auto& entry) {return selectedRow(*command,entry.second)!=command->slots.size();});
    RuntimeAccess::count(candidate)-=command->slots.size();
  } else {
    for(const auto& alias:command->aliases) {
      auto& cache=alias.content?RuntimeAccess::contents(candidate):RuntimeAccess::names(candidate);
      if(!cache.emplace(alias.key,alias.slot).second) runtimeInvalid("Runtime restore would steal a newer alias");
    }
    RuntimeAccess::count(candidate)+=command->slots.size();
  }
  RuntimeAccess::revision(candidate)=table.revision()+1;staged->bytes=budget.bytes;
  auto result=RuntimeAccess::prepared(std::move(staged));
  if(!CanExchangeRuntimeTextureRetirement(result,table)) runtimeInvalid("Prepared runtime retirement has a stale witness or insufficient final-check work budget");
  return result;
}
}

RuntimeTextureRetirementCommand::operator bool() const noexcept {return bool(state_);}
bool RuntimeTextureRetirementCommand::published() const noexcept {return state_ && state_->published;}
bool RuntimeTextureRetirementCommand::retired() const noexcept {return published() && state_->retired;}
uint64_t RuntimeTextureRetirementCommand::ownedBytes() const noexcept {return state_?state_->bytes:0;}
std::span<const uint32_t> RuntimeTextureRetirementCommand::slots() const noexcept {return state_?std::span<const uint32_t>(state_->slots):std::span<const uint32_t>{};}
PreparedRuntimeTextureRetirement::PreparedRuntimeTextureRetirement() noexcept=default;
PreparedRuntimeTextureRetirement::~PreparedRuntimeTextureRetirement()=default;
PreparedRuntimeTextureRetirement::PreparedRuntimeTextureRetirement(PreparedRuntimeTextureRetirement&&) noexcept=default;
PreparedRuntimeTextureRetirement& PreparedRuntimeTextureRetirement::operator=(PreparedRuntimeTextureRetirement&&) noexcept=default;
PreparedRuntimeTextureRetirement::operator bool() const noexcept {return state_ && !state_->consumed;}
const TextureManager& PreparedRuntimeTextureRetirement::textures() const {if(!*this) runtimeInvalid("Runtime retirement candidate is empty or consumed");return state_->candidate;}
RuntimeTextureRetirementCommand PreparedRuntimeTextureRetirement::command() const noexcept {return state_?state_->receipt:RuntimeTextureRetirementCommand{};}
uint64_t PreparedRuntimeTextureRetirement::ownedBytes() const noexcept {return state_?state_->bytes:0;}
PreparedRuntimeTextureRetirement PrepareRuntimeTextureRetirement(const TextureManager& table,std::span<const uint32_t> slots,const RuntimeTextureRetirementLimits& limits) {
  return prepareRuntime(table,slots,{},RuntimeTextureRetirementTarget::Retired,limits);
}
PreparedRuntimeTextureRetirement PrepareRuntimeTextureRetirementReplay(const TextureManager& table,const RuntimeTextureRetirementCommand& receipt,
    RuntimeTextureRetirementTarget target,const RuntimeTextureRetirementLimits& limits) {
  if(!receipt) runtimeInvalid("Runtime replay requires its original retirement receipt");return prepareRuntime(table,{},receipt,target,limits);
}
bool CanExchangeRuntimeTextureRetirement(const PreparedRuntimeTextureRetirement& prepared,const TextureManager& table) noexcept {
  const auto& state=RuntimeAccess::get(prepared);if(!state || state->consumed || state->owner!=&table || !state->receipt || table.revision()!=state->revision || table.revision()==UINT64_MAX) return false;
  const auto& command=*RuntimeAccess::get(state->receipt);const auto& candidate=state->candidate;RuntimeCheck check{state->limits.maxWork};
  if(command.version!=state->commandVersion || command.retired==state->retiring || command.published==state->initial ||
      candidate.textureCount()!=table.textureCount() || RuntimeAccess::owner(candidate)!=RuntimeAccess::owner(table) ||
      RuntimeAccess::vacancy(candidate)!=RuntimeAccess::vacancy(table) ||
      candidate.revision()!=table.revision()+1 || !runtimeEndpoint(table,command,state->retiring,check)) return false;
  const auto expectedCount=state->retiring?candidate.activeTextureCount()+command.slots.size():table.activeTextureCount()+command.slots.size();
  if(expectedCount!=(state->retiring?table.activeTextureCount():candidate.activeTextureCount())) return false;
  for(uint32_t slot=0;slot<table.textureCount();++slot) if(selectedRow(command,slot)==command.slots.size()) {
    if(!check.take() || table.isTextureActive(slot)!=candidate.isTextureActive(slot)) return false;
    const auto& old=RuntimeAccess::identities(table)[slot];const auto& next=RuntimeAccess::identities(candidate)[slot];
    if(old.incarnation!=next.incarnation || old.retiredBy!=next.retiredBy || old.phaseVersion!=next.phaseVersion ||
        !sameResource(RuntimeAccess::rows(table)[slot],RuntimeAccess::rows(candidate)[slot],check)) return false;
  }
  for(const auto content:{false,true}) {
    const auto& old=content?RuntimeAccess::contents(table):RuntimeAccess::names(table);
    const auto& next=content?RuntimeAccess::contents(candidate):RuntimeAccess::names(candidate);size_t changed{};
    for(const auto& alias:command.aliases) if(alias.content==content) ++changed;
    if(state->retiring?(old.size()!=next.size()+changed):(next.size()!=old.size()+changed)) return false;
    for(const auto& [key,slot]:old) {
      if(!check.take(1+key.size())) return false;if(selectedAlias(command,content,key)) continue;
      const auto found=next.find(key);if(found==next.end() || found->second!=slot) return false;
    }
  }
  return true;
}
bool ExchangeRuntimeTextureRetirement(PreparedRuntimeTextureRetirement& prepared,TextureManager& table) noexcept {
  if(!CanExchangeRuntimeTextureRetirement(prepared,table)) return false;auto& state=*RuntimeAccess::get(prepared);auto& command=*RuntimeAccess::get(state.receipt);
  using std::swap;swap(table,state.candidate);command.published=true;command.retired=state.retiring;++command.version;
  for(auto& row:command.rows) ++row.phaseVersion;state.consumed=true;return true;
}

namespace {
bool sameTable(const TextureManager& a,const TextureManager& b,RuntimeCheck& check) noexcept {
  if(a.textureCount()!=b.textureCount() || a.activeTextureCount()!=b.activeTextureCount() || a.revision()!=b.revision() ||
      RuntimeAccess::owner(a)!=RuntimeAccess::owner(b) || RuntimeAccess::vacancy(a)!=RuntimeAccess::vacancy(b)) return false;
  for(size_t row=0;row<a.textureCount();++row) {
    if(!check.take() || a.isTextureActive(uint32_t(row))!=b.isTextureActive(uint32_t(row))) return false;
    const auto& x=RuntimeAccess::identities(a)[row];const auto& y=RuntimeAccess::identities(b)[row];
    if(x.incarnation!=y.incarnation || x.retiredBy!=y.retiredBy || x.phaseVersion!=y.phaseVersion ||
        !sameResource(RuntimeAccess::rows(a)[row],RuntimeAccess::rows(b)[row],check)) return false;
  }
  for(const auto content:{false,true}) {
    const auto& x=content?RuntimeAccess::contents(a):RuntimeAccess::names(a);const auto& y=content?RuntimeAccess::contents(b):RuntimeAccess::names(b);
    if(x.size()!=y.size()) return false;for(const auto& [key,slot]:x) {
      if(!check.take(1+key.size())) return false;const auto found=y.find(key);if(found==y.end() || found->second!=slot) return false;
    }
  }
  return true;
}
bool tailEndpoint(const TextureManager& table,const RuntimeAccess::Append& command,bool present,RuntimeCheck& check) noexcept {
  if(RuntimeAccess::owner(table)!=command.ownerIdentity || table.textureCount()!=uint64_t(command.first)+(present?command.rows.size():0) ||
      (command.first && RuntimeAccess::identities(table)[command.first-1].incarnation!=command.boundary) ||
      (!present && RuntimeAccess::vacancy(table)!=command.vacancy)) return false;
  if(present) for(size_t i=0;i<command.rows.size();++i) {
    const auto row=command.first+i;const auto& identity=RuntimeAccess::identities(table)[row];
    if(!check.take() || !table.isTextureActive(uint32_t(row)) || identity.incarnation!=command.rows[i].incarnation || identity.retiredBy ||
        !sameResource(RuntimeAccess::rows(table)[row],command.rows[i].resource,check)) return false;
  }
  return cacheEndpoint(table,command,present,check);
}
void captureTail(RuntimeAccess::Append& command,const TextureManager& table,uint32_t first,RuntimeBudget& budget) {
  command.first=first;command.ownerIdentity=RuntimeAccess::owner(table);command.identity=std::make_shared<const uint8_t>(0);
  if(first) command.boundary=RuntimeAccess::identities(table)[first-1].incarnation;
  const auto count=table.textureCount()-first;budget.add(count,sizeof(RuntimeSavedRow)+sizeof(uint32_t));
  command.rows.reserve(count);command.slots.reserve(count);
  for(uint32_t row=first;row<table.textureCount();++row) {
    const auto& identity=RuntimeAccess::identities(table)[row];if(!table.isTextureActive(row) || identity.retiredBy || identity.phaseVersion)
      runtimeInvalid("Initial runtime append must publish fresh active row identities");
    const auto& value=RuntimeAccess::rows(table)[row];budget.text(value.name);budget.text(value.contentKey);
    command.slots.push_back(row);command.rows.push_back({value,identity.incarnation,identity.phaseVersion});
  }
  for(const auto* cache:{&RuntimeAccess::names(table),&RuntimeAccess::contents(table)}) for(const auto& [key,row]:*cache) if(row>=first) {
    budget.add(sizeof(RuntimeAlias)+128);budget.text(key);command.aliases.push_back({cache==&RuntimeAccess::contents(table),key,row});
  }
  budget.visit(command.aliases.size()*(4+std::bit_width(command.aliases.size())));
  std::ranges::sort(command.aliases,[](const auto& a,const auto& b) {return a.content!=b.content?a.content<b.content:a.key<b.key;});
  command.bytes=sizeof(RuntimeAccess::Append)+256+command.slots.capacity()*sizeof(uint32_t)+command.rows.capacity()*sizeof(RuntimeSavedRow)+command.aliases.capacity()*sizeof(RuntimeAlias);
  for(const auto& row:command.rows) command.bytes+=row.resource.name.size()+row.resource.contentKey.size()+2;
  for(const auto& alias:command.aliases) command.bytes+=alias.key.size()+1;
}
}
RuntimeTextureAppendCommand::operator bool() const noexcept {return bool(state_);}
uint64_t RuntimeTextureAppendCommand::ownedBytes() const noexcept {return state_?state_->bytes:0;}
PreparedRuntimeTextureAppend::PreparedRuntimeTextureAppend() noexcept=default;
PreparedRuntimeTextureAppend::~PreparedRuntimeTextureAppend()=default;
PreparedRuntimeTextureAppend::PreparedRuntimeTextureAppend(PreparedRuntimeTextureAppend&&) noexcept=default;
PreparedRuntimeTextureAppend& PreparedRuntimeTextureAppend::operator=(PreparedRuntimeTextureAppend&&) noexcept=default;
PreparedRuntimeTextureAppend::operator bool() const noexcept {return state_ && !state_->consumed;}
const TextureManager& PreparedRuntimeTextureAppend::textures() const {if(!*this) runtimeInvalid("Runtime append candidate is consumed or empty");return state_->candidate;}
RuntimeTextureAppendCommand PreparedRuntimeTextureAppend::command() const noexcept {return state_?state_->receipt:RuntimeTextureAppendCommand{};}
uint64_t PreparedRuntimeTextureAppend::ownedBytes() const noexcept {return state_?state_->bytes:0;}
PreparedRuntimeTextureAppend PrepareRuntimeTextureAppend(const TextureManager& before,const TextureManager& appended,const RuntimeTextureRetirementLimits& limits) {
  RuntimeBudget budget(limits);budget.add(sizeof(RuntimeAccess::AppendPrepared)+sizeof(RuntimeAccess::Append)+384);tableCost(before,budget);tableCost(appended,budget);
  if(before.textureCount()>=appended.textureCount() || before.revision()==UINT64_MAX ||
      appended.textureCount()-before.textureCount()>UINT64_MAX-before.revision() ||
      appended.revision()!=before.revision()+appended.textureCount()-before.textureCount() ||
      (RuntimeAccess::owner(before) && RuntimeAccess::owner(before)!=RuntimeAccess::owner(appended)))
    runtimeInvalid("Runtime append must extend its exact original prefix");
  RuntimeCheck check{limits.maxWork};
  for(size_t row=0;row<before.textureCount();++row) {
    const auto& x=RuntimeAccess::identities(before)[row];const auto& y=RuntimeAccess::identities(appended)[row];
    if(x.incarnation!=y.incarnation || x.retiredBy!=y.retiredBy || x.phaseVersion!=y.phaseVersion ||
        before.isTextureActive(uint32_t(row))!=appended.isTextureActive(uint32_t(row)) ||
        !sameResource(RuntimeAccess::rows(before)[row],RuntimeAccess::rows(appended)[row],check)) runtimeInvalid("Runtime append replaced a prefix identity/payload");
  }
  for(const auto content:{false,true}) {
    const auto& old=content?RuntimeAccess::contents(before):RuntimeAccess::names(before);const auto& next=content?RuntimeAccess::contents(appended):RuntimeAccess::names(appended);
    for(const auto& [key,row]:old) {const auto found=next.find(key);if(found==next.end() || found->second!=row) runtimeInvalid("Runtime append replaced a prefix alias");}
    for(const auto& [key,row]:next) if(row<before.textureCount()) {const auto found=old.find(key);if(found==old.end() || found->second!=row) runtimeInvalid("Runtime append invented a prefix alias");}
  }
  budget.visit(limits.maxWork-check.remaining);
  auto command=std::make_shared<RuntimeAccess::Append>();captureTail(*command,appended,uint32_t(before.textureCount()),budget);
  auto state=std::make_unique<RuntimeAccess::AppendPrepared>();state->owner=&before;state->before=before;state->candidate=appended;state->limits=limits;
  state->receipt=RuntimeAccess::append(std::move(command));state->initial=true;state->present=true;state->bytes=budget.bytes;
  auto result=RuntimeAccess::appendPrepared(std::move(state));if(!CanExchangeRuntimeTextureAppend(result,before)) runtimeInvalid("Runtime append snapshot became stale");return result;
}
PreparedRuntimeTextureAppend PrepareRuntimeTextureAppendReplay(const TextureManager& table,const RuntimeTextureAppendCommand& receipt,bool present,const RuntimeTextureRetirementLimits& limits) {
  RuntimeBudget budget(limits);budget.add(sizeof(RuntimeAccess::AppendPrepared)+128);tableCost(table,budget);tableCost(table,budget);
  if(!receipt) runtimeInvalid("Runtime tail replay requires its original append receipt");const auto& command=*RuntimeAccess::get(receipt);
  RuntimeCheck check{limits.maxWork};if(!command.published || command.present==present || command.version==UINT64_MAX || table.revision()==UINT64_MAX ||
      !tailEndpoint(table,command,!present,check)) runtimeInvalid("Runtime tail belongs to another phase, namespace or incarnation");
  budget.visit(limits.maxWork-check.remaining);budget.add(command.bytes);budget.add(command.rows.size(),sizeof(uint64_t));
  if(present) {
    budget.add(command.rows.size(),TextureManager::rowStorageBytes());
    for(const auto& row:command.rows) {budget.text(row.resource.name);budget.text(row.resource.contentKey);}
    for(const auto& alias:command.aliases) {budget.add(sizeof(RuntimeAlias)+128);budget.text(alias.key);}
  }
  auto state=std::make_unique<RuntimeAccess::AppendPrepared>();state->owner=&table;state->before=table;state->limits=limits;
  state->receipt=receipt;state->commandVersion=command.version;state->present=present;
  if(present) {
    auto& next=state->candidate;const auto total=table.textureCount()+command.rows.size();
    RuntimeAccess::rows(next).reserve(total);RuntimeAccess::active(next).reserve(total);RuntimeAccess::identities(next).reserve(total);
    RuntimeAccess::rows(next).assign(RuntimeAccess::rows(table).begin(),RuntimeAccess::rows(table).end());
    RuntimeAccess::active(next).assign(RuntimeAccess::active(table).begin(),RuntimeAccess::active(table).end());
    RuntimeAccess::identities(next).assign(RuntimeAccess::identities(table).begin(),RuntimeAccess::identities(table).end());
    RuntimeAccess::owner(next)=RuntimeAccess::owner(table);RuntimeAccess::count(next)=table.activeTextureCount();
    for(const auto content:{false,true}) {
      const auto& old=content?RuntimeAccess::contents(table):RuntimeAccess::names(table);auto& cache=content?RuntimeAccess::contents(next):RuntimeAccess::names(next);
      const auto added=std::count_if(command.aliases.begin(),command.aliases.end(),[&](const auto& alias){return alias.content==content;});
      cache.reserve(old.size()+size_t(added));cache.insert(old.begin(),old.end());
    }
    for(const auto& row:command.rows) {
      RuntimeAccess::rows(next).push_back(row.resource);RuntimeAccess::active(next).push_back(1);
      RuntimeAccess::identities(next).push_back({row.incarnation,{},row.phaseVersion});
    }
    for(const auto& alias:command.aliases) {
      auto& cache=alias.content?RuntimeAccess::contents(next):RuntimeAccess::names(next);
      if(!cache.emplace(alias.key,alias.slot).second) runtimeInvalid("Runtime tail restore would steal an alias");
    }
    RuntimeAccess::count(next)+=command.rows.size();RuntimeAccess::vacancy(next)=command.resumeVacancy;
  } else {
    state->versions.reserve(command.rows.size());for(const auto slot:command.slots) state->versions.push_back(RuntimeAccess::identities(table)[slot].phaseVersion);
    state->candidate=table.copyPrefix(command.first);
  }
  RuntimeAccess::revision(state->candidate)=table.revision()+1;state->bytes=budget.bytes;
  auto result=RuntimeAccess::appendPrepared(std::move(state));if(!CanExchangeRuntimeTextureAppend(result,table)) runtimeInvalid("Runtime tail snapshot became stale");return result;
}
bool CanExchangeRuntimeTextureAppend(const PreparedRuntimeTextureAppend& prepared,const TextureManager& table) noexcept {
  const auto& state=RuntimeAccess::get(prepared);if(!state || state->consumed || state->owner!=&table || !state->receipt || table.revision()==UINT64_MAX) return false;
  const auto& command=*RuntimeAccess::get(state->receipt);RuntimeCheck check{state->limits.maxWork};
  if(command.version!=state->commandVersion || command.published==state->initial || command.present==state->present ||
      !sameTable(table,state->before,check)) return false;
  if(state->initial) return state->present && state->candidate.textureCount()==uint64_t(command.first)+command.rows.size();
  return tailEndpoint(table,command,!state->present,check) &&
      state->candidate.textureCount()==uint64_t(command.first)+(state->present?command.rows.size():0) &&
      (state->present || state->versions.size()==command.rows.size());
}
bool ExchangeRuntimeTextureAppend(PreparedRuntimeTextureAppend& prepared,TextureManager& table) noexcept {
  if(!CanExchangeRuntimeTextureAppend(prepared,table)) return false;auto& state=*RuntimeAccess::get(prepared);auto& command=*RuntimeAccess::get(state.receipt);
  if(!state.present) {
    for(size_t i=0;i<command.rows.size();++i) command.rows[i].phaseVersion=state.versions[i];
    // A later child may already be detached at this endpoint. Restore that
    // exact current child marker with the original parent, rather than its
    // birth marker; unrelated replacement cuts still have a different token.
    command.resumeVacancy=RuntimeAccess::vacancy(state.before);
    command.vacancy=RuntimeAccess::vacancy(state.candidate);
  }
  using std::swap;swap(table,state.candidate);command.published=true;command.present=state.present;++command.version;state.consumed=true;return true;
}

}  // namespace container::material
