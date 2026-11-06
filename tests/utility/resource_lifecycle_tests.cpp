#include "Container/utility/ResourceLifecycle.h"

#include <gtest/gtest.h>

// Defines allocation replacements and three shared control tests. Include in
// this executable's single translation unit only, outside the namespace.
#include "../renderer/scene/material_publication_allocations.h"

#include <array>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {
using namespace container::util;
using Kind=ResourceKind;
using DomainKind=ResourceDomainKind;
using Pins=ResourcePinClass;
using Phase=ResourceLifecyclePhase;

struct Fixture {
  ResourceLifecycleCatalog catalog;
  ResourceLifecycleDomain archive=catalog.createDomain(DomainKind::Archive);
  ResourceHandle first=catalog.issue(archive,Kind::Texture,0);
  ResourceHandle second=catalog.issue(archive,Kind::Texture,1);
};
std::span<const ResourceHandle> one(const ResourceHandle& handle) {return {&handle,1};}
ResourceRetirementCommand retire(ResourceLifecycleCatalog& owner,const ResourceHandle& handle) {
  auto prepared=owner.prepareRetirement(one(handle),"Delete unused texture");
  auto command=prepared.command();EXPECT_TRUE(owner.exchange(prepared));return command;
}
const ResourceInventoryRow* find(const ResourceCatalogSnapshot& snapshot,const ResourceHandle& handle) {
  for(const auto& row:snapshot.rows()) if(row.resource==handle) return &row;
  return nullptr;
}

TEST(ResourceLifecycle, EmptyDefaultsCannotMintAuthority) {
  ResourceLifecycleCatalog catalog;
  ResourceHandle empty;
  EXPECT_FALSE(empty);EXPECT_FALSE(catalog.contains(empty));EXPECT_FALSE(catalog.active(empty));
  EXPECT_FALSE(catalog.reclaimable(empty));EXPECT_EQ(empty.slot(),UINT32_MAX);EXPECT_EQ(empty.incarnation(),0u);
  EXPECT_THROW(catalog.issue({},Kind::Texture,0),std::invalid_argument);
  EXPECT_THROW(catalog.prepareChange({},Phase::Active),std::invalid_argument);
  PreparedResourceRegistration registration;PreparedResourceTransition transition;
  EXPECT_FALSE(catalog.exchange(registration));EXPECT_FALSE(catalog.exchange(transition));
}

TEST(ResourceLifecycle, AllKindsAndGenerationDomainsAreIndependent) {
  ResourceLifecycleCatalog catalog;
  const auto archive=catalog.createDomain(DomainKind::Archive);
  const auto otherArchive=catalog.createDomain(DomainKind::Archive);
  const auto evaluated=catalog.createDomain(DomainKind::Evaluated);
  const auto runtime=catalog.createDomain(DomainKind::Runtime);
  std::vector<ResourceRegistrationRequest> requests;
  for(const auto& domain:{archive,otherArchive,evaluated,runtime})
    for(size_t kind=0;kind<size_t(Kind::Count);++kind) requests.push_back({domain,Kind(kind),0});
  const auto revision=catalog.revision();auto registration=catalog.prepareIssue(requests);
  EXPECT_EQ(catalog.revision(),revision);EXPECT_TRUE(catalog.inventory().rows().empty());
  ASSERT_EQ(registration.resources().size(),20u);
  for(const auto& handle:registration.resources()) EXPECT_FALSE(catalog.contains(handle));
  ASSERT_TRUE(catalog.exchange(registration));EXPECT_EQ(catalog.revision(),revision+1);
  EXPECT_EQ(catalog.inventory().rows().size(),20u);
  for(const auto& handle:registration.resources()) EXPECT_TRUE(catalog.active(handle));
  EXPECT_NE(archive,otherArchive);EXPECT_EQ(archive.kind(),DomainKind::Archive);
}

TEST(ResourceLifecycle, RegistrationIsAtomicAbandonableAndOneShot) {
  Fixture f;const auto revision=f.catalog.revision();
  ResourceHandle abandoned;
  {
    const std::array requests{ResourceRegistrationRequest{f.archive,Kind::Texture,2},
        ResourceRegistrationRequest{f.archive,Kind::Material,0}};
    auto prepared=f.catalog.prepareIssue(requests);abandoned=prepared.resources()[0];
    EXPECT_FALSE(f.catalog.contains(abandoned));EXPECT_EQ(f.catalog.inventory().rows().size(),2u);
  }
  EXPECT_EQ(f.catalog.revision(),revision);EXPECT_EQ(f.catalog.inventory().rows().size(),2u);
  const auto material=f.catalog.issue(f.archive,Kind::Material,0);
  EXPECT_NE(material,abandoned);EXPECT_FALSE(f.catalog.contains(abandoned));
  const ResourceRegistrationRequest request{f.archive,Kind::Texture,2};auto prepared=f.catalog.prepareIssue(std::span(&request,1));
  ASSERT_TRUE(f.catalog.canExchange(prepared));ASSERT_TRUE(f.catalog.exchange(prepared));
  EXPECT_FALSE(prepared);EXPECT_FALSE(f.catalog.canExchange(prepared));EXPECT_FALSE(f.catalog.exchange(prepared));
}

TEST(ResourceLifecycle, RegistrationRejectsHolesDuplicatesForeignDomainsAndStaleStages) {
  Fixture f;ResourceLifecycleCatalog foreign;const auto domain=foreign.createDomain(DomainKind::Archive);
  EXPECT_THROW(f.catalog.issue(domain,Kind::Texture,0),std::invalid_argument);
  EXPECT_THROW(f.catalog.issue(f.archive,Kind::Texture,3),std::invalid_argument);
  EXPECT_THROW(f.catalog.issue(f.archive,Kind::Count,0),std::invalid_argument);
  const std::array duplicate{ResourceRegistrationRequest{f.archive,Kind::Texture,2},ResourceRegistrationRequest{f.archive,Kind::Texture,2}};
  EXPECT_THROW(f.catalog.prepareIssue(duplicate),std::invalid_argument);
  const ResourceRegistrationRequest request{f.archive,Kind::Texture,2};auto prepared=f.catalog.prepareIssue(std::span(&request,1));
  EXPECT_FALSE(foreign.canExchange(prepared));
  (void)f.catalog.issue(f.archive,Kind::Image,0);
  EXPECT_FALSE(f.catalog.canExchange(prepared));EXPECT_FALSE(f.catalog.exchange(prepared));
  EXPECT_FALSE(f.catalog.contains(prepared.resources().front()));
}

TEST(ResourceLifecycle, PinsAreSortedUniqueClassAwareAndReleasedByMoves) {
  Fixture f;const std::array input{f.second,f.first,f.second,f.first};
  auto pin=f.catalog.pin(input,Pins::History,"Undo material binding");ASSERT_EQ(pin.resources().size(),2u);
  EXPECT_EQ(pin.resources()[0],f.first);EXPECT_EQ(pin.resources()[1],f.second);
  auto moved=std::move(pin);EXPECT_FALSE(pin);ASSERT_TRUE(moved);
  auto snapshot=f.catalog.inventory();ASSERT_EQ(snapshot.reasons().size(),1u);
  EXPECT_EQ(snapshot.reasons()[0].reason,"Undo material binding");EXPECT_EQ(snapshot.reasons()[0].pinClass,Pins::History);
  EXPECT_FALSE(snapshot.reasons()[0].retirementCommand);EXPECT_EQ(snapshot.reasons()[0].resources.size(),2u);
  ASSERT_NE(find(snapshot,f.first),nullptr);EXPECT_EQ(find(snapshot,f.first)->pins[size_t(Pins::History)],1u);
  moved.reset();auto after=f.catalog.inventory();EXPECT_TRUE(after.reasons().empty());
  EXPECT_EQ(find(after,f.first)->pins[size_t(Pins::History)],0u);
  EXPECT_EQ(find(snapshot,f.first)->pins[size_t(Pins::History)],1u); // immutable old diagnostics
}

TEST(ResourceLifecycle, LiveDependenciesBlockButHistoryAndReadersPermitRetirement) {
  Fixture f;auto live=f.catalog.pin(one(f.first),Pins::LiveDependency,"Hidden BIM point object");
  EXPECT_THROW(f.catalog.prepareRetirement(one(f.first),"Delete texture"),std::invalid_argument);
  live.reset();auto history=f.catalog.pin(one(f.first),Pins::History,"Material undo");
  auto gpu=f.catalog.pin(one(f.first),Pins::GpuReader,"Submitted descriptor generation");
  auto command=retire(f.catalog,f.first);EXPECT_FALSE(f.catalog.active(f.first));EXPECT_FALSE(f.catalog.reclaimable(f.first));
  EXPECT_THROW(f.catalog.pin(one(f.first),Pins::LiveDependency,"New object"),std::invalid_argument);
  command={};EXPECT_FALSE(f.catalog.reclaimable(f.first));history.reset();EXPECT_FALSE(f.catalog.reclaimable(f.first));
  gpu.reset();EXPECT_TRUE(f.catalog.reclaimable(f.first));
}

TEST(ResourceLifecycle, PreparationBlockingPolicyIsExplicitAndInvalidatesOldStages) {
  Fixture f;auto external=f.catalog.pin(one(f.first),Pins::Preparation,"Incoming material plan");
  EXPECT_THROW(f.catalog.prepareRetirement(one(f.first),"Delete texture"),std::invalid_argument);
  auto first=f.catalog.prepareRetirement(one(f.second),"Delete other texture");
  auto replacement=f.catalog.prepareRetirement(one(f.first),"Invalidate old preparation",ResourceRetirementPolicy::InvalidatePreparations);
  ASSERT_TRUE(f.catalog.exchange(replacement));EXPECT_FALSE(f.catalog.canExchange(first));
  EXPECT_FALSE(f.catalog.exchange(first));EXPECT_TRUE(f.catalog.active(f.second));EXPECT_FALSE(f.catalog.reclaimable(f.first));
}

TEST(ResourceLifecycle, LivePinAcquiredAfterPreparationRejectsWithoutMutation) {
  Fixture f;auto prepared=f.catalog.prepareRetirement(one(f.first),"Delete texture");
  const auto revision=f.catalog.revision();auto live=f.catalog.pin(one(f.first),Pins::LiveDependency,"New provider object");
  EXPECT_FALSE(f.catalog.canExchange(prepared));EXPECT_FALSE(f.catalog.exchange(prepared));
  EXPECT_EQ(f.catalog.revision(),revision);EXPECT_TRUE(f.catalog.active(f.first));
  live.reset();EXPECT_TRUE(f.catalog.exchange(prepared));
}

TEST(ResourceLifecycle, MultiResourceRetirementValidatesEveryRowBeforePublication) {
  Fixture f;const std::array repeated{f.second,f.first,f.second};
  auto prepared=f.catalog.prepareRetirement(repeated,"Delete selected resources");
  ASSERT_EQ(prepared.command().resources().size(),2u);const auto revision=f.catalog.revision();
  auto live=f.catalog.pin(one(f.second),Pins::LiveDependency,"Last resource acquired a user");
  EXPECT_FALSE(f.catalog.exchange(prepared));EXPECT_TRUE(f.catalog.active(f.first));EXPECT_TRUE(f.catalog.active(f.second));
  EXPECT_EQ(f.catalog.revision(),revision);live.reset();ASSERT_TRUE(f.catalog.exchange(prepared));
  EXPECT_FALSE(f.catalog.active(f.first));EXPECT_FALSE(f.catalog.active(f.second));
}

TEST(ResourceLifecycle, CommandRetainsIncarnationAndReplaysAgainstCurrentCatalog) {
  Fixture f;auto command=retire(f.catalog,f.first);auto copy=command;
  EXPECT_TRUE(command.published());EXPECT_EQ(command.phase(),Phase::Retired);
  EXPECT_THROW(f.catalog.issue(f.archive,Kind::Texture,0),std::invalid_argument);
  (void)f.catalog.issue(f.archive,Kind::Sampler,0); // unrelated registration commutes with fresh replay
  auto undo=f.catalog.prepareChange(copy,Phase::Active);ASSERT_TRUE(f.catalog.exchange(undo));
  EXPECT_TRUE(f.catalog.active(f.first));EXPECT_EQ(command.phase(),Phase::Active);
  EXPECT_FALSE(f.catalog.exchange(undo));
  auto redo=f.catalog.prepareChange(command,Phase::Retired);ASSERT_TRUE(f.catalog.exchange(redo));
  EXPECT_FALSE(f.catalog.active(f.first));EXPECT_EQ(copy.phase(),Phase::Retired);
}

TEST(ResourceLifecycle, OldCommandCannotResurrectAnotherCommandsRetirement) {
  Fixture f;auto old=retire(f.catalog,f.first);
  {auto undo=f.catalog.prepareChange(old,Phase::Active);ASSERT_TRUE(f.catalog.exchange(undo));}
  auto current=retire(f.catalog,f.first);
  EXPECT_THROW(f.catalog.prepareChange(old,Phase::Retired),std::invalid_argument);
  EXPECT_THROW(f.catalog.prepareChange(old,Phase::Active),std::invalid_argument);
  auto undo=f.catalog.prepareChange(current,Phase::Active);EXPECT_TRUE(f.catalog.exchange(undo));
}

TEST(ResourceLifecycle, RetirementAuthorityCannotBeMintedFromWeakHandleOrUnpublishedCommand) {
  Fixture f;auto prepared=f.catalog.prepareRetirement(one(f.first),"Delete texture");auto command=prepared.command();
  EXPECT_FALSE(command.published());EXPECT_THROW(f.catalog.prepareChange(command,Phase::Retired),std::invalid_argument);
  ResourceLifecycleCatalog other;
  EXPECT_FALSE(other.canExchange(prepared));EXPECT_FALSE(other.exchange(prepared));
  ASSERT_TRUE(f.catalog.exchange(prepared));EXPECT_THROW(other.prepareChange(command,Phase::Active),std::invalid_argument);
}

TEST(ResourceLifecycle, ReuseMintsNewIncarnationAndOldWeakSnapshotDoesNotPin) {
  Fixture f;auto snapshot=f.catalog.inventory();const auto old=f.first;
  {auto command=retire(f.catalog,old);EXPECT_FALSE(f.catalog.reclaimable(old));}
  ASSERT_TRUE(f.catalog.reclaimable(old));const auto replacement=f.catalog.issue(f.archive,Kind::Texture,0);
  EXPECT_NE(replacement,old);EXPECT_NE(replacement.incarnation(),old.incarnation());
  EXPECT_EQ(replacement.slot(),old.slot());EXPECT_FALSE(f.catalog.contains(old));EXPECT_TRUE(f.catalog.active(replacement));
  EXPECT_THROW(f.catalog.pin(one(old),Pins::History,"Stale request"),std::invalid_argument);
  ASSERT_NE(find(snapshot,old),nullptr);EXPECT_TRUE(find(snapshot,old)->active);
}

TEST(ResourceLifecycle, RetentionAcquiredAfterReusePreparationRejectsRegistration) {
  Fixture f;{auto command=retire(f.catalog,f.first);}
  const ResourceRegistrationRequest request{f.archive,Kind::Texture,0};auto prepared=f.catalog.prepareIssue(std::span(&request,1));
  auto retained=f.catalog.pin(one(f.first),Pins::History,"Late retained snapshot");
  const auto revision=f.catalog.revision();EXPECT_FALSE(f.catalog.exchange(prepared));EXPECT_TRUE(f.catalog.contains(f.first));
  EXPECT_FALSE(f.catalog.active(f.first));EXPECT_EQ(f.catalog.revision(),revision);
  retained.reset();EXPECT_TRUE(f.catalog.exchange(prepared));EXPECT_FALSE(f.catalog.contains(f.first));
}

TEST(ResourceLifecycle, RetainedSnapshotCopiesShareOneHistoryPinAndDoNotRestoreBitmaps) {
  Fixture f;auto snapshot=f.catalog.retainSnapshot(Pins::History,"Inactive scene generation");auto copy=snapshot;
  ASSERT_NE(find(snapshot,f.first),nullptr);EXPECT_EQ(find(snapshot,f.first)->pins[size_t(Pins::History)],1u);
  {auto command=retire(f.catalog,f.first);}
  EXPECT_FALSE(f.catalog.reclaimable(f.first));EXPECT_TRUE(find(snapshot,f.first)->active);
  snapshot={};EXPECT_FALSE(f.catalog.reclaimable(f.first));copy={};EXPECT_TRUE(f.catalog.reclaimable(f.first));
}

TEST(ResourceLifecycle, ReplayingSelectedCommandDoesNotCompareUnrelatedSnapshotBitmap) {
  Fixture f;auto retained=f.catalog.retainSnapshot(Pins::History,"Old scene snapshot");
  auto first=retire(f.catalog,f.first);auto second=retire(f.catalog,f.second);
  auto undo=f.catalog.prepareChange(first,Phase::Active);EXPECT_TRUE(f.catalog.exchange(undo));
  EXPECT_TRUE(f.catalog.active(f.first));EXPECT_FALSE(f.catalog.active(f.second));
  EXPECT_TRUE(find(retained,f.second)->active);EXPECT_EQ(second.phase(),Phase::Retired);
}

TEST(ResourceLifecycle, PinsAndCommandsCanOutliveOwnerWithoutDanglingRelease) {
  ResourcePin pin;ResourceRetirementCommand command;ResourceCatalogSnapshot snapshot;
  {
    auto owner=std::make_unique<ResourceLifecycleCatalog>();auto domain=owner->createDomain(DomainKind::Runtime);
    auto handle=owner->issue(domain,Kind::RuntimeTexture,0);
    pin=owner->pin(one(handle),Pins::GpuReader,"Retired frame");command=retire(*owner,handle);
    snapshot=owner->retainSnapshot(Pins::History,"Retained diagnostic owner");
  }
  EXPECT_TRUE(pin);EXPECT_TRUE(command);EXPECT_TRUE(snapshot);pin.reset();command={};snapshot={};
}

TEST(ResourceLifecycle, InvalidEnumsEmptyInventoriesAndInvalidLimitsReject) {
  ResourceLifecycleLimits limits;limits.maxWork=0;EXPECT_THROW(ResourceLifecycleCatalog rejected(limits),std::invalid_argument);
  limits={};limits.maxOwnedBytes=UINT64_MAX;EXPECT_THROW(ResourceLifecycleCatalog rejected(limits),std::invalid_argument);
  Fixture f;
  EXPECT_THROW(f.catalog.pin({},Pins::History,"Empty"),std::invalid_argument);
  EXPECT_THROW(f.catalog.pin(one(f.first),Pins::Count,"Invalid"),std::invalid_argument);
  EXPECT_THROW(f.catalog.pin(one(f.first),Pins::History,""),std::invalid_argument);
  EXPECT_THROW(f.catalog.createDomain(DomainKind::Count),std::invalid_argument);
  EXPECT_THROW(f.catalog.prepareRetirement(one(f.first),"Invalid",ResourceRetirementPolicy(99)),std::invalid_argument);
  EXPECT_THROW(f.catalog.prepareIssue({}),std::invalid_argument);
}

TEST(ResourceLifecycle, AggregatePinCountAndReferenceLimitsReleaseWithOwner) {
  ResourceLifecycleLimits limits;limits.maxPinSets=1;limits.maxPinnedReferences=1;
  ResourceLifecycleCatalog catalog(limits);auto domain=catalog.createDomain(DomainKind::Archive);
  auto first=catalog.issue(domain,Kind::Image,0),second=catalog.issue(domain,Kind::Image,1);
  auto pin=catalog.pin(one(first),Pins::History,"One");
  EXPECT_THROW(catalog.pin(one(second),Pins::History,"Two"),std::length_error);
  pin.reset();auto next=catalog.pin(one(second),Pins::History,"Two");EXPECT_TRUE(next);
  next.reset();const std::array duplicate{first,first};EXPECT_EQ(catalog.pin(duplicate,Pins::History,"Unique").resources().size(),1u);
}

TEST(ResourceLifecycle, DomainSlotRegistrationAndSnapshotLimitsAreAggregate) {
  ResourceLifecycleLimits limits;limits.maxDomains=1;limits.maxSlots=2;limits.maxRegistrations=1;
  limits.maxRegistrationReferences=1;limits.maxSnapshots=1;limits.maxSnapshotRows=2;
  ResourceLifecycleCatalog catalog(limits);auto domain=catalog.createDomain(DomainKind::Archive);
  EXPECT_THROW(catalog.createDomain(DomainKind::Runtime),std::length_error);
  auto first=catalog.issue(domain,Kind::Texture,0);
  const ResourceRegistrationRequest request{domain,Kind::Image,0};auto pending=catalog.prepareIssue(std::span(&request,1));
  EXPECT_THROW(catalog.prepareIssue(std::span(&request,1)),std::length_error);
  ASSERT_TRUE(catalog.exchange(pending));pending={};
  EXPECT_THROW(catalog.issue(domain,Kind::Sampler,0),std::length_error);
  auto snapshot=catalog.inventory();EXPECT_THROW(catalog.inventory(),std::length_error);snapshot={};
  EXPECT_EQ(catalog.inventory().rows().size(),2u);EXPECT_TRUE(catalog.active(first));
}

TEST(ResourceLifecycle, CommandLimitsAndFailureDoNotLeakPinsOrRevision) {
  ResourceLifecycleLimits limits;limits.maxCommands=1;ResourceLifecycleCatalog catalog(limits);
  auto domain=catalog.createDomain(DomainKind::Archive);auto a=catalog.issue(domain,Kind::Texture,0),b=catalog.issue(domain,Kind::Texture,1);
  auto command=retire(catalog,a);const auto revision=catalog.revision();
  EXPECT_THROW(catalog.prepareRetirement(one(b),"Other command"),std::length_error);
  EXPECT_EQ(catalog.revision(),revision);EXPECT_TRUE(catalog.active(b));command={};
  auto prepared=catalog.prepareRetirement(one(b),"Other command");EXPECT_TRUE(catalog.exchange(prepared));
}

TEST(ResourceLifecycle, WorkAndByteBoundsRejectBeforePublishing) {
  ResourceLifecycleLimits limits;limits.maxWork=1;ResourceLifecycleCatalog catalog(limits);
  auto domain=catalog.createDomain(DomainKind::Archive);const auto revision=catalog.revision();
  EXPECT_THROW(catalog.issue(domain,Kind::Texture,0),std::length_error);
  EXPECT_EQ(catalog.revision(),revision);EXPECT_THROW(catalog.inventory(),std::length_error);
  Fixture f;const auto bytes=f.catalog.ownedBytes();
  {auto pin=f.catalog.pin(one(f.first),Pins::History,"Temporary");EXPECT_GT(f.catalog.ownedBytes(),bytes);}
  EXPECT_EQ(f.catalog.ownedBytes(),bytes);
  {auto snapshot=f.catalog.inventory();EXPECT_GT(f.catalog.ownedBytes(),bytes);}
  EXPECT_EQ(f.catalog.ownedBytes(),bytes);
}

TEST(ResourceLifecycle, ByteLimitFailureReleasesAllTemporaryOwners) {
  ResourceLifecycleLimits limits;limits.maxOwnedBytes=4096;limits.maxReasonBytes=8192;
  ResourceLifecycleCatalog catalog(limits);auto domain=catalog.createDomain(DomainKind::Archive);
  auto handle=catalog.issue(domain,Kind::Texture,0);const auto bytes=catalog.ownedBytes(),revision=catalog.revision();
  const std::string largeReason(4096,'x');
  EXPECT_THROW(catalog.pin(one(handle),Pins::History,largeReason),std::length_error);
  EXPECT_EQ(catalog.ownedBytes(),bytes);EXPECT_EQ(catalog.revision(),revision);EXPECT_TRUE(catalog.active(handle));
}

TEST(ResourceLifecycle, AbandonedRetirementReleasesPreparationAndCommandOwners) {
  Fixture f;const auto bytes=f.catalog.ownedBytes(),revision=f.catalog.revision();
  {auto prepared=f.catalog.prepareRetirement(one(f.first),"Abandoned deletion");EXPECT_TRUE(prepared);}
  EXPECT_EQ(f.catalog.ownedBytes(),bytes);EXPECT_EQ(f.catalog.revision(),revision);EXPECT_TRUE(f.catalog.active(f.first));
  auto snapshot=f.catalog.inventory();ASSERT_NE(find(snapshot,f.first),nullptr);
  EXPECT_EQ(find(snapshot,f.first)->commandOwners,0u);
  EXPECT_EQ(find(snapshot,f.first)->pins[size_t(Pins::Preparation)],0u);
}

TEST(ResourceLifecycle, MixedRegistrationRetirementAndResurrectionPublishWithoutAllocating) {
  Fixture f;const auto evaluated=f.catalog.createDomain(DomainKind::Evaluated);
  const auto runtime=f.catalog.createDomain(DomainKind::Runtime);
  const std::array requests{ResourceRegistrationRequest{f.archive,Kind::Material,0},
      ResourceRegistrationRequest{f.archive,Kind::Texture,2},
      ResourceRegistrationRequest{evaluated,Kind::Image,0},
      ResourceRegistrationRequest{runtime,Kind::RuntimeTexture,0}};
  auto registration=f.catalog.prepareIssue(requests);
  bool ready{},published{};size_t attempts{};
  {
    material_publication_test::AllocationGuard guard(true);
    ready=f.catalog.canExchange(registration);published=f.catalog.exchange(registration);attempts=guard.allocations();
  }
  EXPECT_TRUE(ready);EXPECT_TRUE(published);EXPECT_EQ(attempts,0u);
  for(const auto& handle:registration.resources()) EXPECT_TRUE(f.catalog.active(handle));

  const std::array selected{f.first,f.second};
  auto retirement=f.catalog.prepareRetirement(selected,"Allocation-free retirement");auto command=retirement.command();
  {
    material_publication_test::AllocationGuard guard(true);
    ready=f.catalog.canExchange(retirement);published=f.catalog.exchange(retirement);attempts=guard.allocations();
  }
  EXPECT_TRUE(ready);EXPECT_TRUE(published);EXPECT_EQ(attempts,0u);
  EXPECT_FALSE(f.catalog.active(f.first));EXPECT_FALSE(f.catalog.active(f.second));

  auto resurrection=f.catalog.prepareChange(command,Phase::Active);
  {
    material_publication_test::AllocationGuard guard(true);
    ready=f.catalog.canExchange(resurrection);published=f.catalog.exchange(resurrection);attempts=guard.allocations();
  }
  EXPECT_TRUE(ready);EXPECT_TRUE(published);EXPECT_EQ(attempts,0u);
  EXPECT_TRUE(f.catalog.active(f.first));EXPECT_TRUE(f.catalog.active(f.second));
}

TEST(ResourceLifecycle, ForeignStaleAndConsumedPublicationRejectWithoutAllocating) {
  Fixture f;ResourceLifecycleCatalog other;
  const ResourceRegistrationRequest request{f.archive,Kind::Texture,2};auto registration=f.catalog.prepareIssue(std::span(&request,1));
  auto retirement=f.catalog.prepareRetirement(one(f.first),"Rejected publication");
  std::array<bool,4> results{};size_t attempts{};
  {
    material_publication_test::AllocationGuard guard(true);
    results={other.canExchange(registration),other.exchange(registration),other.canExchange(retirement),other.exchange(retirement)};
    attempts=guard.allocations();
  }
  EXPECT_EQ(results,(std::array<bool,4>{false,false,false,false}));EXPECT_EQ(attempts,0u);
  (void)f.catalog.issue(f.archive,Kind::Sampler,0);const auto revision=f.catalog.revision();
  {
    material_publication_test::AllocationGuard guard(true);
    results={f.catalog.canExchange(registration),f.catalog.exchange(registration),f.catalog.canExchange(retirement),f.catalog.exchange(retirement)};
    attempts=guard.allocations();
  }
  EXPECT_EQ(results,(std::array<bool,4>{false,false,false,false}));EXPECT_EQ(attempts,0u);
  EXPECT_EQ(f.catalog.revision(),revision);EXPECT_TRUE(f.catalog.active(f.first));
  // Release stale preparation before making a fresh default-policy command.
  retirement={};registration={};
  auto current=f.catalog.prepareRetirement(one(f.first),"Consumed publication");ASSERT_TRUE(f.catalog.exchange(current));
  {
    material_publication_test::AllocationGuard guard(true);
    results[0]=f.catalog.canExchange(current);results[1]=f.catalog.exchange(current);attempts=guard.allocations();
  }
  EXPECT_FALSE(results[0]);EXPECT_FALSE(results[1]);EXPECT_EQ(attempts,0u);
}

TEST(ResourceLifecycle, StagedPinsPublishEveryClassAndMixedHandlesWithRegistrationWithoutAllocating) {
  Fixture f;const auto runtime=f.catalog.createDomain(DomainKind::Runtime);
  const std::array requests{ResourceRegistrationRequest{f.archive,Kind::Material,0},
      ResourceRegistrationRequest{runtime,Kind::RuntimeTexture,0}};
  auto registration=f.catalog.prepareIssue(requests);const auto material=registration.resources()[0],image=registration.resources()[1];
  const auto revision=f.catalog.revision();
  const std::array repeated{material,f.first,material};
  auto live=f.catalog.stagePin(registration,repeated,Pins::LiveDependency,"Future mesh material dependency");
  auto history=f.catalog.stagePin(registration,one(material),Pins::History,"Future material history");
  auto gpu=f.catalog.stagePin(registration,one(image),Pins::GpuReader,"Future descriptor generation");
  auto preparation=f.catalog.stagePin(registration,one(image),Pins::Preparation,"Future resource preparation");
  EXPECT_TRUE(live);EXPECT_TRUE(live.pending());EXPECT_FALSE(live.active());EXPECT_EQ(live.resources().size(),2u);
  EXPECT_THROW(f.catalog.pin(one(material),Pins::History,"Unpublished weak intent"),std::invalid_argument);
  auto before=f.catalog.inventory();ASSERT_EQ(before.rows().size(),2u);EXPECT_TRUE(before.reasons().empty());
  EXPECT_EQ(find(before,f.first)->pins[size_t(Pins::LiveDependency)],0u);EXPECT_EQ(f.catalog.revision(),revision);
  bool can{},published{};size_t attempts{};
  {
    material_publication_test::AllocationGuard guard(true);
    can=f.catalog.canExchange(registration);published=f.catalog.exchange(registration);attempts=guard.allocations();
  }
  EXPECT_TRUE(can);EXPECT_TRUE(published);EXPECT_EQ(attempts,0u);EXPECT_EQ(f.catalog.revision(),revision+1);
  EXPECT_TRUE(live.active());EXPECT_FALSE(live.pending());EXPECT_TRUE(history.active());EXPECT_TRUE(gpu.active());EXPECT_TRUE(preparation.active());
  auto after=f.catalog.inventory();ASSERT_NE(find(after,material),nullptr);ASSERT_NE(find(after,image),nullptr);
  EXPECT_EQ(find(after,material)->pins[size_t(Pins::LiveDependency)],1u);EXPECT_EQ(find(after,material)->pins[size_t(Pins::History)],1u);
  EXPECT_EQ(find(after,f.first)->pins[size_t(Pins::LiveDependency)],1u);
  EXPECT_EQ(find(after,image)->pins[size_t(Pins::GpuReader)],1u);EXPECT_EQ(find(after,image)->pins[size_t(Pins::Preparation)],1u);
  EXPECT_EQ(after.reasons().size(),4u);EXPECT_EQ(find(before,f.first)->pins[size_t(Pins::LiveDependency)],0u);
}

TEST(ResourceLifecycle, AbandonedRegistrationCancelsEscapedTokensWithoutPublishingOwnership) {
  Fixture f;ResourcePin escaped;ResourceHandle candidate;const auto revision=f.catalog.revision();
  {
    const ResourceRegistrationRequest request{f.archive,Kind::Texture,2};auto registration=f.catalog.prepareIssue(std::span(&request,1));
    candidate=registration.resources()[0];const std::array selected{f.first,candidate};
    escaped=f.catalog.stagePin(registration,selected,Pins::LiveDependency,"Abandoned successor");
    EXPECT_TRUE(escaped.pending());EXPECT_FALSE(escaped.active());
  }
  EXPECT_FALSE(escaped);EXPECT_FALSE(escaped.pending());EXPECT_FALSE(escaped.active());EXPECT_TRUE(escaped.resources().empty());
  EXPECT_FALSE(f.catalog.contains(candidate));EXPECT_EQ(f.catalog.revision(),revision);
  auto snapshot=f.catalog.inventory();EXPECT_EQ(snapshot.rows().size(),2u);EXPECT_TRUE(snapshot.reasons().empty());
  EXPECT_EQ(find(snapshot,f.first)->pins[size_t(Pins::LiveDependency)],0u);
}

TEST(ResourceLifecycle, DroppingPendingPinDetachesItAndMovedRegistrationStillCommits) {
  Fixture f;const ResourceRegistrationRequest request{f.archive,Kind::Image,0};auto original=f.catalog.prepareIssue(std::span(&request,1));
  const auto image=original.resources()[0];auto dropped=f.catalog.stagePin(original,one(image),Pins::History,"Dropped candidate pin");
  auto retained=f.catalog.stagePin(original,one(image),Pins::GpuReader,"Moved candidate pin");
  ResourcePin movedPin=std::move(retained);PreparedResourceRegistration moved=std::move(original);
  EXPECT_FALSE(original);EXPECT_FALSE(retained);EXPECT_TRUE(movedPin.pending());dropped.reset();
  bool published{};size_t attempts{};
  {material_publication_test::AllocationGuard guard(true);published=f.catalog.exchange(moved);attempts=guard.allocations();}
  EXPECT_TRUE(published);EXPECT_EQ(attempts,0u);EXPECT_TRUE(movedPin.active());
  auto snapshot=f.catalog.inventory();ASSERT_NE(find(snapshot,image),nullptr);
  EXPECT_EQ(find(snapshot,image)->pins[size_t(Pins::History)],0u);EXPECT_EQ(find(snapshot,image)->pins[size_t(Pins::GpuReader)],1u);
}

TEST(ResourceLifecycle, PendingPinCapacitySurvivesInterleavedPublishedPinCreation) {
  Fixture f;const ResourceRegistrationRequest request{f.archive,Kind::Texture,2};auto registration=f.catalog.prepareIssue(std::span(&request,1));
  const auto candidate=registration.resources()[0];auto pending=f.catalog.stagePin(registration,one(candidate),Pins::History,"Pending history");
  auto published=f.catalog.pin(one(f.first),Pins::History,"Published history created later");
  auto second=f.catalog.stagePin(registration,one(f.second),Pins::GpuReader,"Pending old reader");
  bool result{};size_t attempts{};
  {material_publication_test::AllocationGuard guard(true);result=f.catalog.exchange(registration);attempts=guard.allocations();}
  EXPECT_TRUE(result);EXPECT_EQ(attempts,0u);EXPECT_TRUE(pending.active());EXPECT_TRUE(published.active());EXPECT_TRUE(second.active());
  auto snapshot=f.catalog.inventory();EXPECT_EQ(snapshot.reasons().size(),3u);
}

TEST(ResourceLifecycle, StagedPinsRejectForeignStaleRetiredAndOtherRegistrationsCandidates) {
  Fixture f;const ResourceRegistrationRequest request{f.archive,Kind::Texture,2};
  auto registration=f.catalog.prepareIssue(std::span(&request,1)),otherRegistration=f.catalog.prepareIssue(std::span(&request,1));
  const auto candidate=registration.resources()[0],otherCandidate=otherRegistration.resources()[0];
  ResourceLifecycleCatalog other;const auto foreignDomain=other.createDomain(DomainKind::Archive);const auto foreign=other.issue(foreignDomain,Kind::Texture,0);
  EXPECT_THROW(other.stagePin(registration,one(candidate),Pins::History,"Foreign owner"),std::invalid_argument);
  EXPECT_THROW(f.catalog.stagePin(registration,one(foreign),Pins::History,"Foreign intent"),std::invalid_argument);
  EXPECT_THROW(f.catalog.stagePin(registration,one(otherCandidate),Pins::History,"Other prepared incarnation"),std::invalid_argument);
  EXPECT_THROW(f.catalog.stagePin(registration,{},Pins::History,"Empty intent"),std::invalid_argument);
  EXPECT_THROW(f.catalog.stagePin(registration,one(candidate),Pins::Count,"Invalid class"),std::invalid_argument);
  {auto command=retire(f.catalog,f.first);}
  EXPECT_THROW(f.catalog.stagePin(registration,one(candidate),Pins::History,"Stale registration"),std::invalid_argument);
  const ResourceRegistrationRequest next{f.archive,Kind::Image,0};auto current=f.catalog.prepareIssue(std::span(&next,1));
  EXPECT_THROW(f.catalog.stagePin(current,one(f.first),Pins::History,"Retired existing handle"),std::invalid_argument);
  EXPECT_TRUE(f.catalog.exchange(current));
  EXPECT_THROW(f.catalog.stagePin(current,one(current.resources()[0]),Pins::History,"Consumed registration"),std::invalid_argument);
}

TEST(ResourceLifecycle, RetirementOfExistingIntentInvalidatesPendingRegistrationWithoutPublishingPins) {
  Fixture f;const ResourceRegistrationRequest request{f.archive,Kind::Texture,2};auto registration=f.catalog.prepareIssue(std::span(&request,1));
  const auto candidate=registration.resources()[0];const std::array selected{candidate,f.first};
  auto pending=f.catalog.stagePin(registration,selected,Pins::LiveDependency,"Future dependency");
  auto command=retire(f.catalog,f.first); // Pending intent is deliberately absent from live dependency counts.
  bool can{},published{};size_t attempts{};
  {material_publication_test::AllocationGuard guard(true);can=f.catalog.canExchange(registration);published=f.catalog.exchange(registration);attempts=guard.allocations();}
  EXPECT_FALSE(can);EXPECT_FALSE(published);EXPECT_EQ(attempts,0u);EXPECT_FALSE(f.catalog.contains(candidate));EXPECT_TRUE(pending.pending());
  auto snapshot=f.catalog.inventory();ASSERT_EQ(snapshot.reasons().size(),1u);EXPECT_TRUE(snapshot.reasons()[0].retirementCommand);
  EXPECT_EQ(find(snapshot,f.first)->pins[size_t(Pins::LiveDependency)],0u);
  registration={};EXPECT_FALSE(pending);EXPECT_FALSE(pending.active());EXPECT_FALSE(pending.pending());
}

TEST(ResourceLifecycle, PendingSetsAndReferencesShareAggregateLimitsAndCancellationReleasesThem) {
  ResourceLifecycleLimits limits;limits.maxPinSets=2;limits.maxPinnedReferences=3;ResourceLifecycleCatalog catalog(limits);
  const auto domain=catalog.createDomain(DomainKind::Archive);const auto existing=catalog.issue(domain,Kind::Texture,0);
  const ResourceRegistrationRequest request{domain,Kind::Texture,1};auto registration=catalog.prepareIssue(std::span(&request,1));
  const auto candidate=registration.resources()[0];const std::array both{candidate,existing};
  auto first=catalog.stagePin(registration,both,Pins::History,"Pending two references");
  auto second=catalog.stagePin(registration,one(candidate),Pins::GpuReader,"Pending third reference");
  EXPECT_THROW(catalog.pin(one(existing),Pins::History,"Full aggregate"),std::length_error);
  EXPECT_THROW(catalog.stagePin(registration,one(candidate),Pins::History,"Full pending aggregate"),std::length_error);
  registration={};EXPECT_FALSE(first);EXPECT_FALSE(second);
  auto normal=catalog.pin(one(existing),Pins::History,"Released pending budget");EXPECT_TRUE(normal.active());
}

TEST(ResourceLifecycle, StagedPinAllocationFailurePreservesPreviouslyStagedOwnerAndRegistration) {
  Fixture f;const ResourceRegistrationRequest request{f.archive,Kind::Texture,2};auto registration=f.catalog.prepareIssue(std::span(&request,1));
  const auto candidate=registration.resources()[0];auto previous=f.catalog.stagePin(registration,one(candidate),Pins::History,"Earlier staged owner");
  const auto bytes=f.catalog.ownedBytes(),revision=f.catalog.revision();bool failed{};size_t attempts{};
  {
    material_publication_test::AllocationGuard guard(true);
    try {(void)f.catalog.stagePin(registration,one(candidate),Pins::GpuReader,"Rejected allocation");}
    catch(const std::bad_alloc&) {failed=true;}
    attempts=guard.allocations();
  }
  EXPECT_TRUE(failed);EXPECT_GT(attempts,0u);EXPECT_EQ(f.catalog.ownedBytes(),bytes);EXPECT_EQ(f.catalog.revision(),revision);
  EXPECT_TRUE(previous.pending());EXPECT_FALSE(previous.active());EXPECT_TRUE(f.catalog.inventory().reasons().empty());
  bool published{};
  {material_publication_test::AllocationGuard guard(true);published=f.catalog.exchange(registration);attempts=guard.allocations();}
  EXPECT_TRUE(published);EXPECT_EQ(attempts,0u);EXPECT_TRUE(previous.active());
}

TEST(ResourceLifecycle, PendingTokenCanOutliveClosedCatalogAndRegistrationWithoutDanglingRelease) {
  ResourcePin escaped;PreparedResourceRegistration registration;ResourceHandle candidate;
  {
    auto catalog=std::make_unique<ResourceLifecycleCatalog>();const auto domain=catalog->createDomain(DomainKind::Runtime);
    const ResourceRegistrationRequest request{domain,Kind::RuntimeTexture,0};registration=catalog->prepareIssue(std::span(&request,1));
    candidate=registration.resources()[0];escaped=catalog->stagePin(registration,one(candidate),Pins::GpuReader,"Escaped future generation");
    EXPECT_TRUE(escaped.pending());catalog.reset();
  }
  EXPECT_FALSE(escaped);EXPECT_FALSE(escaped.pending());EXPECT_FALSE(escaped.active());EXPECT_TRUE(escaped.resources().empty());
  EXPECT_FALSE(registration);ResourceLifecycleCatalog other;bool result{};size_t attempts{};
  {material_publication_test::AllocationGuard guard(true);result=other.exchange(registration);attempts=guard.allocations();}
  EXPECT_FALSE(result);EXPECT_EQ(attempts,0u);registration={};escaped.reset();
}

TEST(ResourceLifecycle, ExactOutgoingOwnersRetireAndRestoreAnEntireDependencyGraphWithoutAllocating) {
  Fixture f;const auto image=f.catalog.issue(f.archive,Kind::Image,0),sampler=f.catalog.issue(f.archive,Kind::Sampler,0);
  const std::array dependencies{image,sampler};const std::array selected{f.first,image,sampler};
  auto outgoing=f.catalog.pin(dependencies,Pins::LiveDependency,"Texture image and sampler edges");
  EXPECT_THROW(f.catalog.prepareRetirement(selected,"Ordinary retirement must still block"),std::invalid_argument);
  const ResourcePin* released[]{&outgoing};
  auto transition=f.catalog.prepareRetirement(selected,released,"Retire selected graph");auto command=transition.command();
  auto history=f.catalog.stagePin(transition,selected,Pins::History,"Selected graph history");
  EXPECT_TRUE(outgoing.active());EXPECT_TRUE(history.pending());EXPECT_FALSE(history.active());
  const auto revision=f.catalog.revision();auto before=f.catalog.inventory();
  EXPECT_EQ(find(before,image)->pins[size_t(Pins::LiveDependency)],1u);
  EXPECT_EQ(find(before,image)->pins[size_t(Pins::History)],0u);
  bool ready{},published{};size_t attempts{};
  {
    material_publication_test::AllocationGuard guard(true);
    ready=f.catalog.canExchange(transition);published=f.catalog.exchange(transition);attempts=guard.allocations();
  }
  EXPECT_TRUE(ready);EXPECT_TRUE(published);EXPECT_EQ(attempts,0u);EXPECT_EQ(f.catalog.revision(),revision+1);
  EXPECT_FALSE(outgoing);EXPECT_TRUE(outgoing.resources().empty());EXPECT_TRUE(history.active());
  for(const auto& handle:selected) EXPECT_FALSE(f.catalog.active(handle));
  auto retired=f.catalog.inventory();EXPECT_EQ(find(retired,image)->pins[size_t(Pins::LiveDependency)],0u);
  EXPECT_EQ(find(retired,image)->pins[size_t(Pins::History)],1u);

  auto restore=f.catalog.prepareChange(command,Phase::Active);
  const std::array restoredRefs{image,sampler,f.second,image};
  auto restored=f.catalog.stagePin(restore,restoredRefs,Pins::LiveDependency,"Restored exact outgoing edges");
  auto reader=f.catalog.stagePin(restore,one(f.first),Pins::GpuReader,"Restored descriptor generation");
  EXPECT_TRUE(restored.pending());EXPECT_EQ(restored.resources().size(),3u);
  EXPECT_THROW(f.catalog.pin(one(image),Pins::LiveDependency,"Early restore edge"),std::invalid_argument);
  {
    material_publication_test::AllocationGuard guard(true);
    ready=f.catalog.canExchange(restore);published=f.catalog.exchange(restore);attempts=guard.allocations();
  }
  EXPECT_TRUE(ready);EXPECT_TRUE(published);EXPECT_EQ(attempts,0u);EXPECT_TRUE(restored.active());EXPECT_TRUE(reader.active());
  for(const auto& handle:selected) EXPECT_TRUE(f.catalog.active(handle));
  auto active=f.catalog.inventory();EXPECT_EQ(find(active,image)->pins[size_t(Pins::LiveDependency)],1u);
  EXPECT_EQ(find(active,f.second)->pins[size_t(Pins::LiveDependency)],1u);
  const ResourcePin* restoredOwner[]{&restored};auto redo=f.catalog.prepareChange(command,Phase::Retired,restoredOwner);
  ASSERT_TRUE(f.catalog.exchange(redo));EXPECT_FALSE(restored);EXPECT_TRUE(f.catalog.active(f.second));
}

TEST(ResourceLifecycle, ResidualAndLateExternalPinsStillBlockExactDependencyRetirement) {
  Fixture f;auto outgoing=f.catalog.pin(one(f.first),Pins::LiveDependency,"Selected outgoing edge");
  auto external=f.catalog.pin(one(f.first),Pins::LiveDependency,"Surviving external object");
  const ResourcePin* released[]{&outgoing};
  EXPECT_THROW(f.catalog.prepareRetirement(one(f.first),released,"External live user blocks"),std::invalid_argument);
  external.reset();auto transition=f.catalog.prepareRetirement(one(f.first),released,"Exact selected release");
  auto pending=f.catalog.stagePin(transition,one(f.first),Pins::History,"Pending history");
  const auto revision=f.catalog.revision();external=f.catalog.pin(one(f.first),Pins::LiveDependency,"Late external object");
  EXPECT_EQ(f.catalog.revision(),revision);
  bool ready{},published{};size_t attempts{};
  {
    material_publication_test::AllocationGuard guard(true);
    ready=f.catalog.canExchange(transition);published=f.catalog.exchange(transition);attempts=guard.allocations();
  }
  EXPECT_FALSE(ready);EXPECT_FALSE(published);EXPECT_EQ(attempts,0u);EXPECT_EQ(f.catalog.revision(),revision);
  EXPECT_TRUE(outgoing.active());EXPECT_TRUE(pending.pending());EXPECT_TRUE(f.catalog.active(f.first));
  external.reset();auto preparation=f.catalog.pin(one(f.first),Pins::Preparation,"Late incoming plan");
  EXPECT_FALSE(f.catalog.canExchange(transition));EXPECT_FALSE(f.catalog.exchange(transition));EXPECT_TRUE(outgoing.active());
  preparation.reset();EXPECT_TRUE(f.catalog.exchange(transition));EXPECT_FALSE(outgoing);EXPECT_TRUE(pending.active());
}

TEST(ResourceLifecycle, MultipleExactOutgoingOwnersExplainAggregateCountsButNeverPreparationPins) {
  Fixture f;const auto image=f.catalog.issue(f.archive,Kind::Image,0);const std::array selected{f.first,f.second,image};
  auto first=f.catalog.pin(one(image),Pins::LiveDependency,"First texture image edge");
  auto second=f.catalog.pin(one(image),Pins::LiveDependency,"Second texture image edge");
  auto preparation=f.catalog.pin(one(image),Pins::Preparation,"Independent incoming texture");
  const ResourcePin* released[]{&first,&second};
  EXPECT_THROW(f.catalog.prepareRetirement(selected,released,"Own edges cannot hide preparation"),std::invalid_argument);
  auto transition=f.catalog.prepareRetirement(selected,released,"Explicitly invalidate incoming preparation",
      ResourceRetirementPolicy::InvalidatePreparations);
  auto before=f.catalog.inventory();EXPECT_EQ(find(before,image)->pins[size_t(Pins::LiveDependency)],2u);
  bool published{};size_t attempts{};
  {material_publication_test::AllocationGuard guard(true);published=f.catalog.exchange(transition);attempts=guard.allocations();}
  EXPECT_TRUE(published);EXPECT_EQ(attempts,0u);EXPECT_FALSE(first);EXPECT_FALSE(second);EXPECT_TRUE(preparation.active());
  auto after=f.catalog.inventory();EXPECT_EQ(find(after,image)->pins[size_t(Pins::LiveDependency)],0u);
  EXPECT_EQ(find(after,image)->pins[size_t(Pins::Preparation)],1u);EXPECT_FALSE(f.catalog.active(image));
}

TEST(ResourceLifecycle, ResetAndEqualReplacementCannotSubstituteCapturedOutgoingAuthority) {
  Fixture f;auto outgoing=f.catalog.pin(one(f.first),Pins::LiveDependency,"Original owner");
  const ResourcePin* released[]{&outgoing};auto transition=f.catalog.prepareRetirement(one(f.first),released,"Captured exact owner");
  auto pending=f.catalog.stagePin(transition,one(f.first),Pins::History,"Must remain pending");
  const auto revision=f.catalog.revision();outgoing.reset();
  outgoing=f.catalog.pin(one(f.first),Pins::LiveDependency,"Same rows but another serial");
  EXPECT_EQ(f.catalog.revision(),revision);EXPECT_FALSE(f.catalog.canExchange(transition));EXPECT_FALSE(f.catalog.exchange(transition));
  EXPECT_TRUE(outgoing.active());EXPECT_TRUE(pending.pending());EXPECT_TRUE(f.catalog.active(f.first));
  transition={};EXPECT_FALSE(pending);EXPECT_TRUE(outgoing.active());
}

TEST(ResourceLifecycle, OutgoingOwnersMayMoveButDuplicateWrongClassPendingAndForeignTokensReject) {
  Fixture f;auto outgoing=f.catalog.pin(one(f.first),Pins::LiveDependency,"Movable exact owner");
  auto history=f.catalog.pin(one(f.first),Pins::History,"History is not outgoing authority");
  ResourceLifecycleCatalog other;const auto domain=other.createDomain(DomainKind::Archive);
  const auto foreignHandle=other.issue(domain,Kind::Image,0);auto foreign=other.pin(one(foreignHandle),Pins::LiveDependency,"Foreign owner");
  ResourcePin empty;const ResourcePin* duplicated[]{&outgoing,&outgoing};const ResourcePin* wrong[]{&history};
  const ResourcePin* foreignPins[]{&foreign};const ResourcePin* emptyPins[]{&empty};const ResourcePin* nullPins[]{nullptr};
  EXPECT_THROW(f.catalog.prepareDependencyChange(duplicated,"Duplicate owners"),std::invalid_argument);
  EXPECT_THROW(f.catalog.prepareDependencyChange(wrong,"Wrong class"),std::invalid_argument);
  EXPECT_THROW(f.catalog.prepareDependencyChange(foreignPins,"Foreign owner"),std::invalid_argument);
  EXPECT_THROW(f.catalog.prepareDependencyChange(emptyPins,"Empty token"),std::invalid_argument);
  EXPECT_THROW(f.catalog.prepareDependencyChange(nullPins,"Null token"),std::invalid_argument);
  auto pendingTransition=f.catalog.prepareDependencyChange({},"Future first edges");
  auto pending=f.catalog.stagePin(pendingTransition,one(f.second),Pins::LiveDependency,"Pending edge");
  const ResourcePin* pendingPins[]{&pending};
  EXPECT_THROW(f.catalog.prepareDependencyChange(pendingPins,"Pending is not active authority"),std::invalid_argument);
  pendingTransition={};const ResourcePin* released[]{&outgoing};
  auto transition=f.catalog.prepareRetirement(one(f.first),released,"Move preserves exact owner");
  ResourcePin moved=std::move(outgoing);EXPECT_TRUE(moved.active());EXPECT_TRUE(f.catalog.exchange(transition));EXPECT_FALSE(moved);
}

TEST(ResourceLifecycle, AbandoningTransitionPreservesOutgoingOwnersAndCancelsEscapedPins) {
  Fixture f;auto outgoing=f.catalog.pin(one(f.first),Pins::LiveDependency,"Original live edge");
  const ResourcePin* released[]{&outgoing};const auto revision=f.catalog.revision(),bytes=f.catalog.ownedBytes();
  ResourcePin escaped;
  {
    auto transition=f.catalog.prepareRetirement(one(f.first),released,"Abandoned graph deletion");
    escaped=f.catalog.stagePin(transition,one(f.second),Pins::LiveDependency,"Abandoned successor edge");
    EXPECT_TRUE(escaped.pending());EXPECT_TRUE(outgoing.active());
  }
  EXPECT_FALSE(escaped);EXPECT_TRUE(escaped.resources().empty());EXPECT_TRUE(outgoing.active());
  EXPECT_TRUE(f.catalog.active(f.first));EXPECT_EQ(f.catalog.revision(),revision);
  escaped.reset();EXPECT_EQ(f.catalog.ownedBytes(),bytes);
  auto snapshot=f.catalog.inventory();ASSERT_EQ(snapshot.reasons().size(),1u);
  EXPECT_EQ(snapshot.reasons()[0].reason,"Original live edge");EXPECT_EQ(find(snapshot,f.first)->pins[size_t(Pins::Preparation)],0u);
}

TEST(ResourceLifecycle, PhaseLessDependencyReplacementPublishesOnlyAtExactNoAllocCommit) {
  Fixture f;auto outgoing=f.catalog.pin(one(f.first),Pins::LiveDependency,"Old material texture");
  const ResourcePin* released[]{&outgoing};auto original=f.catalog.prepareDependencyChange(released,"Material texture replacement");
  EXPECT_FALSE(original.command());auto dropped=f.catalog.stagePin(original,one(f.first),Pins::History,"Dropped future history");
  const std::array duplicated{f.second,f.second};auto pending=f.catalog.stagePin(original,duplicated,Pins::LiveDependency,"New material texture");
  ResourcePin movedPin=std::move(pending);PreparedResourceTransition moved=std::move(original);dropped.reset();
  auto interleaved=f.catalog.pin(one(f.second),Pins::GpuReader,"Existing descriptor reader");
  const auto revision=f.catalog.revision();auto before=f.catalog.inventory();
  EXPECT_EQ(find(before,f.first)->pins[size_t(Pins::LiveDependency)],1u);
  EXPECT_EQ(find(before,f.second)->pins[size_t(Pins::LiveDependency)],0u);EXPECT_TRUE(movedPin.pending());
  bool ready{},published{};size_t attempts{};
  {
    material_publication_test::AllocationGuard guard(true);
    ready=f.catalog.canExchange(moved);published=f.catalog.exchange(moved);attempts=guard.allocations();
  }
  EXPECT_TRUE(ready);EXPECT_TRUE(published);EXPECT_EQ(attempts,0u);EXPECT_EQ(f.catalog.revision(),revision+1);
  EXPECT_FALSE(original);EXPECT_FALSE(moved);EXPECT_FALSE(outgoing);EXPECT_TRUE(movedPin.active());EXPECT_TRUE(interleaved.active());
  EXPECT_TRUE(f.catalog.active(f.first));EXPECT_TRUE(f.catalog.active(f.second));
  auto after=f.catalog.inventory();EXPECT_EQ(find(after,f.first)->pins[size_t(Pins::LiveDependency)],0u);
  EXPECT_EQ(find(after,f.second)->pins[size_t(Pins::LiveDependency)],1u);
  EXPECT_FALSE(f.catalog.exchange(moved));
  auto first=f.catalog.prepareDependencyChange({},"First new outgoing ownership");
  auto firstPin=f.catalog.stagePin(first,one(f.first),Pins::LiveDependency,"First edge");
  ASSERT_TRUE(f.catalog.exchange(first));EXPECT_TRUE(firstPin.active());
}

TEST(ResourceLifecycle, TransitionStagingRejectsRetiredNonselectedRowsAndRetiringLivePins) {
  Fixture f;auto command=retire(f.catalog,f.first);auto transition=f.catalog.prepareRetirement(one(f.second),"Retire second");
  EXPECT_THROW(f.catalog.stagePin(transition,one(f.second),Pins::LiveDependency,"Cannot leave retired live"),std::invalid_argument);
  EXPECT_THROW(f.catalog.stagePin(transition,one(f.first),Pins::History,"Other retired identity"),std::invalid_argument);
  EXPECT_THROW(f.catalog.stagePin(transition,{},Pins::History,"Empty"),std::invalid_argument);
  EXPECT_THROW(f.catalog.stagePin(transition,one(f.second),Pins::Count,"Bad class"),std::invalid_argument);
  ResourceLifecycleCatalog foreign;
  EXPECT_THROW(foreign.stagePin(transition,one(f.second),Pins::History,"Wrong catalog"),std::invalid_argument);
  auto retained=f.catalog.stagePin(transition,one(f.second),Pins::Preparation,"Post-retirement retained plan");
  EXPECT_TRUE(f.catalog.exchange(transition));EXPECT_TRUE(retained.active());
  EXPECT_THROW(f.catalog.stagePin(transition,one(f.second),Pins::History,"Consumed transition"),std::invalid_argument);
  auto phaseLess=f.catalog.prepareDependencyChange({},"Retired refs reject");
  EXPECT_THROW(f.catalog.stagePin(phaseLess,one(f.first),Pins::LiveDependency,"Retired dependency"),std::invalid_argument);
  EXPECT_THROW(f.catalog.stagePin(phaseLess,one(f.first),Pins::History,"Retired noncandidate"),std::invalid_argument);
}

TEST(ResourceLifecycle, RestorePendingPinsRemainInvisibleAndCancelAfterUnrelatedPhaseChange) {
  Fixture f;auto command=retire(f.catalog,f.first);auto restore=f.catalog.prepareChange(command,Phase::Active);
  auto pending=f.catalog.stagePin(restore,one(f.first),Pins::LiveDependency,"Future restored dependency");
  auto other=retire(f.catalog,f.second);const auto revision=f.catalog.revision();
  EXPECT_FALSE(f.catalog.canExchange(restore));EXPECT_FALSE(f.catalog.exchange(restore));EXPECT_TRUE(pending.pending());
  EXPECT_FALSE(f.catalog.active(f.first));EXPECT_EQ(f.catalog.revision(),revision);
  auto snapshot=f.catalog.inventory();EXPECT_EQ(find(snapshot,f.first)->pins[size_t(Pins::LiveDependency)],0u);
  restore={};EXPECT_FALSE(pending);EXPECT_TRUE(pending.resources().empty());
}

TEST(ResourceLifecycle, TransitionPendingLimitsAndAllocationFailuresPreserveOldOwnership) {
  ResourceLifecycleLimits limits;limits.maxPinSets=3;limits.maxPinnedReferences=3;ResourceLifecycleCatalog catalog(limits);
  const auto domain=catalog.createDomain(DomainKind::Archive);const auto first=catalog.issue(domain,Kind::Texture,0);
  const auto second=catalog.issue(domain,Kind::Texture,1);auto outgoing=catalog.pin(one(first),Pins::LiveDependency,"Old");
  const ResourcePin* released[]{&outgoing};auto transition=catalog.prepareDependencyChange(released,"Bounded replacement");
  auto pending=catalog.stagePin(transition,one(second),Pins::LiveDependency,"New");
  const auto bytes=catalog.ownedBytes(),revision=catalog.revision();bool failed{};size_t attempts{};
  {
    material_publication_test::AllocationGuard guard(true);
    try {(void)catalog.stagePin(transition,one(second),Pins::History,"Allocation failure");}
    catch(const std::bad_alloc&) {failed=true;}
    attempts=guard.allocations();
  }
  EXPECT_TRUE(failed);EXPECT_GT(attempts,0u);EXPECT_EQ(catalog.ownedBytes(),bytes);EXPECT_EQ(catalog.revision(),revision);
  EXPECT_TRUE(outgoing.active());EXPECT_TRUE(pending.pending());
  auto full=catalog.stagePin(transition,one(second),Pins::History,"Third aggregate reference");
  EXPECT_THROW(catalog.stagePin(transition,one(first),Pins::History,"Exceeds pending cap"),std::length_error);
  EXPECT_THROW(catalog.pin(one(first),Pins::History,"Exceeds combined cap"),std::length_error);
  transition={};EXPECT_FALSE(pending);EXPECT_FALSE(full);EXPECT_TRUE(outgoing.active());
  auto replacement=catalog.pin(one(second),Pins::LiveDependency,"Released pending budget");EXPECT_TRUE(replacement.active());
}

TEST(ResourceLifecycle, OutgoingCaptureAllocationFailureAndCatalogClosePreserveSafeTokens) {
  Fixture f;auto outgoing=f.catalog.pin(one(f.first),Pins::LiveDependency,"Old owner");const ResourcePin* released[]{&outgoing};
  const auto bytes=f.catalog.ownedBytes(),revision=f.catalog.revision();bool failed{};size_t attempts{};
  {
    material_publication_test::AllocationGuard guard(true);
    try {(void)f.catalog.prepareRetirement(one(f.first),released,"Fail capture allocation");}
    catch(const std::bad_alloc&) {failed=true;}
    attempts=guard.allocations();
  }
  EXPECT_TRUE(failed);EXPECT_GT(attempts,0u);EXPECT_EQ(f.catalog.ownedBytes(),bytes);EXPECT_EQ(f.catalog.revision(),revision);
  EXPECT_TRUE(outgoing.active());EXPECT_TRUE(f.catalog.active(f.first));
  ResourcePin escaped,old;PreparedResourceTransition retained;
  {
    auto catalog=std::make_unique<ResourceLifecycleCatalog>();const auto domain=catalog->createDomain(DomainKind::Runtime);
    const auto handle=catalog->issue(domain,Kind::RuntimeTexture,0);old=catalog->pin(one(handle),Pins::LiveDependency,"Closing old edge");
    const ResourcePin* owners[]{&old};retained=catalog->prepareDependencyChange(owners,"Closing replacement");
    escaped=catalog->stagePin(retained,one(handle),Pins::LiveDependency,"Escaped future edge");catalog.reset();
  }
  EXPECT_FALSE(retained);EXPECT_FALSE(escaped);EXPECT_TRUE(escaped.resources().empty());EXPECT_TRUE(old.active());
  ResourceLifecycleCatalog other;bool published{};
  {material_publication_test::AllocationGuard guard(true);published=other.exchange(retained);attempts=guard.allocations();}
  EXPECT_FALSE(published);EXPECT_EQ(attempts,0u);retained={};escaped.reset();old.reset();
}

TEST(ResourceLifecycle, OneRegistrationReleasesExactOldOwnersAndPublishesNewAndExistingDependencyTargets) {
  Fixture f;auto first=f.catalog.pin(one(f.first),Pins::LiveDependency,"Old first material edge");
  auto second=f.catalog.pin(one(f.second),Pins::LiveDependency,"Old second role edge");
  auto external=f.catalog.pin(one(f.first),Pins::LiveDependency,"Independent surviving owner");
  const std::array requests{ResourceRegistrationRequest{f.archive,Kind::Texture,2},ResourceRegistrationRequest{f.archive,Kind::Image,0}};
  auto registration=f.catalog.prepareIssue(requests);const auto image=registration.resources()[0],texture=registration.resources()[1];
  const std::array targets{texture,image,f.second,texture};
  auto incoming=f.catalog.stagePin(registration,targets,Pins::LiveDependency,"Successor resource and material edges");
  const ResourcePin* oldFirst[]{&first};f.catalog.stageDependencyRelease(registration,oldFirst);
  auto history=f.catalog.stagePin(registration,one(texture),Pins::History,"New texture history");
  const ResourcePin* oldSecond[]{&second};f.catalog.stageDependencyRelease(registration,oldSecond);
  const auto bytes=f.catalog.ownedBytes(),revision=f.catalog.revision();f.catalog.stageDependencyRelease(registration,{});
  EXPECT_EQ(f.catalog.ownedBytes(),bytes);EXPECT_EQ(f.catalog.revision(),revision);
  ResourcePin movedOld=std::move(second),movedIncoming=std::move(incoming);
  PreparedResourceRegistration moved=std::move(registration);
  EXPECT_TRUE(first.active());EXPECT_TRUE(movedOld.active());EXPECT_TRUE(movedIncoming.pending());
  auto before=f.catalog.inventory();EXPECT_EQ(find(before,f.first)->pins[size_t(Pins::LiveDependency)],2u);
  EXPECT_EQ(find(before,f.second)->pins[size_t(Pins::LiveDependency)],1u);EXPECT_FALSE(f.catalog.contains(texture));
  bool ready{},published{};size_t attempts{};
  {
    material_publication_test::AllocationGuard guard(true);
    ready=f.catalog.canExchange(moved);published=f.catalog.exchange(moved);attempts=guard.allocations();
  }
  EXPECT_TRUE(ready);EXPECT_TRUE(published);EXPECT_EQ(attempts,0u);EXPECT_EQ(f.catalog.revision(),revision+1);
  EXPECT_FALSE(first);EXPECT_FALSE(movedOld);EXPECT_TRUE(external.active());EXPECT_TRUE(movedIncoming.active());EXPECT_TRUE(history.active());
  auto after=f.catalog.inventory();EXPECT_EQ(find(after,f.first)->pins[size_t(Pins::LiveDependency)],1u);
  EXPECT_EQ(find(after,f.second)->pins[size_t(Pins::LiveDependency)],1u);
  EXPECT_EQ(find(after,texture)->pins[size_t(Pins::LiveDependency)],1u);EXPECT_EQ(find(after,image)->pins[size_t(Pins::LiveDependency)],1u);
  EXPECT_THROW(f.catalog.stageDependencyRelease(moved,{}),std::invalid_argument);
}

TEST(ResourceLifecycle, RegistrationReleaseRejectsEqualReplacementWithoutPublishingRowsOrPendingPins) {
  Fixture f;auto old=f.catalog.pin(one(f.first),Pins::LiveDependency,"Captured old owner");
  const ResourceRegistrationRequest request{f.archive,Kind::Image,0};auto registration=f.catalog.prepareIssue(std::span(&request,1));
  const auto candidate=registration.resources()[0];auto pending=f.catalog.stagePin(registration,one(candidate),Pins::LiveDependency,"Pending new edge");
  const ResourcePin* released[]{&old};f.catalog.stageDependencyRelease(registration,released);
  const auto revision=f.catalog.revision();old.reset();old=f.catalog.pin(one(f.first),Pins::LiveDependency,"Equal rows different owner serial");
  bool ready{},published{};size_t attempts{};
  {
    material_publication_test::AllocationGuard guard(true);
    ready=f.catalog.canExchange(registration);published=f.catalog.exchange(registration);attempts=guard.allocations();
  }
  EXPECT_FALSE(ready);EXPECT_FALSE(published);EXPECT_EQ(attempts,0u);EXPECT_EQ(f.catalog.revision(),revision);
  EXPECT_TRUE(old.active());EXPECT_TRUE(pending.pending());EXPECT_FALSE(f.catalog.contains(candidate));
  EXPECT_THROW(f.catalog.stageDependencyRelease(registration,{}),std::invalid_argument);
  registration={};EXPECT_FALSE(pending);EXPECT_TRUE(old.active());
}

TEST(ResourceLifecycle, RepeatedReleaseStagingRejectsDuplicatesWrongClassesForeignAndPendingOwnersAtomically) {
  Fixture f;auto first=f.catalog.pin(one(f.first),Pins::LiveDependency,"First owner");
  auto second=f.catalog.pin(one(f.second),Pins::LiveDependency,"Second owner");auto history=f.catalog.pin(one(f.first),Pins::History,"History owner");
  const ResourceRegistrationRequest request{f.archive,Kind::Image,0};auto registration=f.catalog.prepareIssue(std::span(&request,1));
  const auto image=registration.resources()[0];auto pending=f.catalog.stagePin(registration,one(image),Pins::LiveDependency,"Pending owner");
  const ResourcePin* firstOnly[]{&first};f.catalog.stageDependencyRelease(registration,firstOnly);
  const auto bytes=f.catalog.ownedBytes();const ResourcePin* duplicated[]{&second,&second};
  const ResourcePin* mixedWrong[]{&second,&history};const ResourcePin* pendingOnly[]{&pending};
  EXPECT_THROW(f.catalog.stageDependencyRelease(registration,firstOnly),std::invalid_argument);
  EXPECT_THROW(f.catalog.stageDependencyRelease(registration,duplicated),std::invalid_argument);
  EXPECT_THROW(f.catalog.stageDependencyRelease(registration,mixedWrong),std::invalid_argument);
  EXPECT_THROW(f.catalog.stageDependencyRelease(registration,pendingOnly),std::invalid_argument);
  ResourceLifecycleCatalog foreign;const auto domain=foreign.createDomain(DomainKind::Archive);
  const auto foreignResource=foreign.issue(domain,Kind::Image,0);auto foreignPin=foreign.pin(one(foreignResource),Pins::LiveDependency,"Foreign owner");
  const ResourcePin* foreignOnly[]{&foreignPin};EXPECT_THROW(f.catalog.stageDependencyRelease(registration,foreignOnly),std::invalid_argument);
  EXPECT_THROW(foreign.stageDependencyRelease(registration,firstOnly),std::invalid_argument);
  EXPECT_EQ(f.catalog.ownedBytes(),bytes);EXPECT_TRUE(first.active());EXPECT_TRUE(second.active());EXPECT_TRUE(pending.pending());
  const ResourcePin* secondOnly[]{&second};f.catalog.stageDependencyRelease(registration,secondOnly);
  ASSERT_TRUE(f.catalog.exchange(registration));EXPECT_FALSE(first);EXPECT_FALSE(second);EXPECT_TRUE(history.active());EXPECT_TRUE(pending.active());
}

TEST(ResourceLifecycle, EveryFallibleReleaseAttachmentPreservesEarlierProofsAndPendingOwnership) {
  size_t attachmentAllocations=0;
  {
    Fixture f;auto first=f.catalog.pin(one(f.first),Pins::LiveDependency,"First owner"),second=f.catalog.pin(one(f.second),Pins::LiveDependency,"Second owner");
    const ResourceRegistrationRequest request{f.archive,Kind::Image,0};auto registration=f.catalog.prepareIssue(std::span(&request,1));
    auto pending=f.catalog.stagePin(registration,registration.resources(),Pins::LiveDependency,"Pending owner");
    const ResourcePin* firstOnly[]{&first};f.catalog.stageDependencyRelease(registration,firstOnly);const ResourcePin* secondOnly[]{&second};
    {material_publication_test::AllocationGuard guard(false);f.catalog.stageDependencyRelease(registration,secondOnly);attachmentAllocations=guard.allocations();}
  }
  ASSERT_GT(attachmentAllocations,0u);ASSERT_LT(attachmentAllocations,16u);
  for(size_t allowance=0;allowance<attachmentAllocations;++allowance) {
    Fixture f;auto first=f.catalog.pin(one(f.first),Pins::LiveDependency,"First owner"),second=f.catalog.pin(one(f.second),Pins::LiveDependency,"Second owner");
    const ResourceRegistrationRequest request{f.archive,Kind::Image,0};auto registration=f.catalog.prepareIssue(std::span(&request,1));
    auto pending=f.catalog.stagePin(registration,registration.resources(),Pins::LiveDependency,"Pending owner");
    const ResourcePin* firstOnly[]{&first};f.catalog.stageDependencyRelease(registration,firstOnly);const ResourcePin* secondOnly[]{&second};
    const auto bytes=f.catalog.ownedBytes(),revision=f.catalog.revision();bool failed{};size_t attempts{};
    {
      material_publication_test::AllocationGuard guard(false,allowance);
      try {f.catalog.stageDependencyRelease(registration,secondOnly);} catch(const std::bad_alloc&) {failed=true;}
      attempts=guard.allocations();
    }
    EXPECT_TRUE(failed);EXPECT_GT(attempts,allowance);EXPECT_EQ(f.catalog.ownedBytes(),bytes);EXPECT_EQ(f.catalog.revision(),revision);
    EXPECT_TRUE(first.active());EXPECT_TRUE(second.active());EXPECT_TRUE(pending.pending());
    bool published{};
    {material_publication_test::AllocationGuard guard(true);published=f.catalog.exchange(registration);attempts=guard.allocations();}
    EXPECT_TRUE(published);EXPECT_EQ(attempts,0u);EXPECT_FALSE(first);EXPECT_TRUE(second.active());EXPECT_TRUE(pending.active());
  }
}

TEST(ResourceLifecycle, AbandonedRegistrationReleasePreservesOldPinsAndCancelsEscapedSuccessors) {
  Fixture f;auto old=f.catalog.pin(one(f.first),Pins::LiveDependency,"Existing material edge");
  const auto bytes=f.catalog.ownedBytes(),revision=f.catalog.revision();ResourcePin escaped;
  {
    const ResourceRegistrationRequest request{f.archive,Kind::Image,0};auto registration=f.catalog.prepareIssue(std::span(&request,1));
    const ResourcePin* oldOnly[]{&old};f.catalog.stageDependencyRelease(registration,oldOnly);
    escaped=f.catalog.stagePin(registration,registration.resources(),Pins::LiveDependency,"Abandoned replacement edge");
    EXPECT_TRUE(old.active());EXPECT_TRUE(escaped.pending());
  }
  EXPECT_TRUE(old.active());EXPECT_FALSE(escaped);EXPECT_EQ(f.catalog.revision(),revision);
  escaped.reset();EXPECT_EQ(f.catalog.ownedBytes(),bytes);
  auto snapshot=f.catalog.inventory();EXPECT_EQ(find(snapshot,f.first)->pins[size_t(Pins::LiveDependency)],1u);
}

TEST(ResourceLifecycle, LateTransitionReleasesRequireCompleteResidualProofAndPreservePreparationPolicy) {
  Fixture f;auto initial=f.catalog.pin(one(f.first),Pins::LiveDependency,"Initial selected edge");const ResourcePin* initialOnly[]{&initial};
  auto transition=f.catalog.prepareRetirement(one(f.first),initialOnly,"Retire graph after paired plan");
  auto pending=f.catalog.stagePin(transition,one(f.first),Pins::History,"Retained selected history");
  auto late=f.catalog.pin(one(f.first),Pins::LiveDependency,"Late exact paired edge");
  auto external=f.catalog.pin(one(f.first),Pins::LiveDependency,"Unrelated surviving edge");const ResourcePin* lateOnly[]{&late};
  const auto bytes=f.catalog.ownedBytes(),revision=f.catalog.revision();
  EXPECT_FALSE(f.catalog.canExchange(transition));
  EXPECT_THROW(f.catalog.stageDependencyRelease(transition,lateOnly),std::invalid_argument);
  EXPECT_EQ(f.catalog.ownedBytes(),bytes);EXPECT_TRUE(initial.active());EXPECT_TRUE(late.active());EXPECT_TRUE(pending.pending());
  external.reset();auto preparation=f.catalog.pin(one(f.first),Pins::Preparation,"Unrelated incoming preparation");
  EXPECT_THROW(f.catalog.stageDependencyRelease(transition,lateOnly),std::invalid_argument);
  preparation.reset();f.catalog.stageDependencyRelease(transition,lateOnly);
  EXPECT_THROW(f.catalog.stageDependencyRelease(transition,initialOnly),std::invalid_argument);
  ResourcePin moved=std::move(late);bool ready{},published{};size_t attempts{};
  {
    material_publication_test::AllocationGuard guard(true);
    ready=f.catalog.canExchange(transition);published=f.catalog.exchange(transition);attempts=guard.allocations();
  }
  EXPECT_TRUE(ready);EXPECT_TRUE(published);EXPECT_EQ(attempts,0u);EXPECT_EQ(f.catalog.revision(),revision+1);
  EXPECT_FALSE(initial);EXPECT_FALSE(moved);EXPECT_TRUE(pending.active());EXPECT_FALSE(f.catalog.active(f.first));
}

TEST(ResourceLifecycle, RestoreAndPhaseLessTransitionsCanAttachReleasesToTheirExistingPendingPins) {
  Fixture f;auto command=retire(f.catalog,f.first);auto old=f.catalog.pin(one(f.second),Pins::LiveDependency,"Old active role edge");
  auto restore=f.catalog.prepareChange(command,Phase::Active);const std::array targets{f.first,f.second};
  auto restored=f.catalog.stagePin(restore,targets,Pins::LiveDependency,"Restored source and prefix edges");
  const ResourcePin* oldOnly[]{&old};f.catalog.stageDependencyRelease(restore,oldOnly);
  bool published{};size_t attempts{};
  {material_publication_test::AllocationGuard guard(true);published=f.catalog.exchange(restore);attempts=guard.allocations();}
  EXPECT_TRUE(published);EXPECT_EQ(attempts,0u);EXPECT_FALSE(old);EXPECT_TRUE(restored.active());EXPECT_TRUE(f.catalog.active(f.first));
  auto replacement=f.catalog.prepareDependencyChange({},"Factor-only replacement");
  auto incoming=f.catalog.stagePin(replacement,one(f.first),Pins::LiveDependency,"Successor factor edge");
  const ResourcePin* restoredOnly[]{&restored};f.catalog.stageDependencyRelease(replacement,restoredOnly);
  f.catalog.stageDependencyRelease(replacement,{});
  {material_publication_test::AllocationGuard guard(true);published=f.catalog.exchange(replacement);attempts=guard.allocations();}
  EXPECT_TRUE(published);EXPECT_EQ(attempts,0u);EXPECT_FALSE(restored);EXPECT_TRUE(incoming.active());
}

TEST(ResourceLifecycle, StagedReleaseDoesNotSpendFutureFreedPinCapacityBeforeCommit) {
  ResourceLifecycleLimits limits;limits.maxPinSets=3;limits.maxPinnedReferences=3;ResourceLifecycleCatalog catalog(limits);
  const auto domain=catalog.createDomain(DomainKind::Archive);const auto first=catalog.issue(domain,Kind::Texture,0),second=catalog.issue(domain,Kind::Texture,1);
  auto oldFirst=catalog.pin(one(first),Pins::LiveDependency,"First old edge"),oldSecond=catalog.pin(one(second),Pins::LiveDependency,"Second old edge");
  const ResourceRegistrationRequest request{domain,Kind::Image,0};auto registration=catalog.prepareIssue(std::span(&request,1));
  const auto image=registration.resources()[0];auto pending=catalog.stagePin(registration,one(image),Pins::LiveDependency,"New edge");
  const ResourcePin* outgoing[]{&oldFirst,&oldSecond};catalog.stageDependencyRelease(registration,outgoing);
  EXPECT_THROW(catalog.stagePin(registration,one(first),Pins::History,"Cannot use future capacity"),std::length_error);
  EXPECT_TRUE(oldFirst.active());EXPECT_TRUE(oldSecond.active());ASSERT_TRUE(catalog.exchange(registration));
  EXPECT_FALSE(oldFirst);EXPECT_FALSE(oldSecond);EXPECT_TRUE(pending.active());
  auto reader=catalog.pin(one(first),Pins::GpuReader,"Released capacity after commit");EXPECT_TRUE(reader.active());
}

TEST(ResourceLifecycle, AppendAuthorityIsBornOnlyFromOriginalUnpublishedCompleteRegistration) {
  Fixture f;ResourceLifecycleCatalog foreign;PreparedResourceRegistration empty;
  EXPECT_THROW(f.catalog.retainAppendAuthority(empty,"No registration"),std::invalid_argument);
  EXPECT_THROW(f.catalog.prepareAppendChange({},Phase::Retired),std::invalid_argument);
  const std::array requests{ResourceRegistrationRequest{f.archive,Kind::Texture,2},ResourceRegistrationRequest{f.archive,Kind::Image,0}};
  auto registration=f.catalog.prepareIssue(requests);const auto revision=f.catalog.revision();
  auto append=f.catalog.retainAppendAuthority(registration,"Original import ownership");
  EXPECT_TRUE(append);EXPECT_FALSE(append.published());EXPECT_EQ(append.resources().size(),2u);
  EXPECT_EQ(f.catalog.revision(),revision);EXPECT_EQ(f.catalog.inventory().rows().size(),2u);
  EXPECT_TRUE(f.catalog.inventory().reasons().empty());
  EXPECT_THROW(f.catalog.retainAppendAuthority(registration,"Duplicate authority"),std::invalid_argument);
  EXPECT_THROW(foreign.retainAppendAuthority(registration,"Foreign registration"),std::invalid_argument);
  EXPECT_THROW(f.catalog.prepareAppendChange(append,Phase::Retired),std::invalid_argument);
  PreparedResourceRegistration moved=std::move(registration);auto history=f.catalog.stagePin(moved,append.resources(),Pins::History,"Original import payload");
  bool ready{},published{};size_t allocations{};
  {material_publication_test::AllocationGuard deny(true);ready=f.catalog.canExchange(moved);published=f.catalog.exchange(moved);allocations=deny.allocations();}
  EXPECT_TRUE(ready);EXPECT_TRUE(published);EXPECT_EQ(allocations,0u);EXPECT_TRUE(append.published());EXPECT_TRUE(history.active());
  EXPECT_THROW(f.catalog.retainAppendAuthority(moved,"Too late"),std::invalid_argument);
  const auto inventory=f.catalog.inventory();for(const auto& handle:append.resources()) {ASSERT_NE(find(inventory,handle),nullptr);EXPECT_EQ(find(inventory,handle)->commandOwners,1u);}
  EXPECT_THROW(foreign.prepareAppendChange(append,Phase::Retired),std::invalid_argument);
}

TEST(ResourceLifecycle, AbandonedAndClosedRegistrationCancelEscapedAppendAuthorityWithoutPublishingRows) {
  Fixture f;ResourceAppendCommand escaped;ResourceHandle abandoned;const auto revision=f.catalog.revision();
  {const ResourceRegistrationRequest request{f.archive,Kind::Image,0};auto registration=f.catalog.prepareIssue(std::span(&request,1));
    abandoned=registration.resources()[0];escaped=f.catalog.retainAppendAuthority(registration,"Canceled import");}
  EXPECT_FALSE(escaped);EXPECT_FALSE(escaped.published());EXPECT_TRUE(escaped.resources().empty());
  EXPECT_EQ(f.catalog.revision(),revision);EXPECT_FALSE(f.catalog.contains(abandoned));
  EXPECT_THROW(f.catalog.prepareAppendChange(escaped,Phase::Retired),std::invalid_argument);
  const auto current=f.catalog.issue(f.archive,Kind::Image,0);EXPECT_NE(current,abandoned);
  ResourceAppendCommand closed;PreparedResourceRegistration pending;
  {auto owner=std::make_unique<ResourceLifecycleCatalog>();const auto domain=owner->createDomain(DomainKind::Runtime);
    const ResourceRegistrationRequest request{domain,Kind::RuntimeTexture,0};pending=owner->prepareIssue(std::span(&request,1));
    closed=owner->retainAppendAuthority(pending,"Owner closed before publication");}
  EXPECT_FALSE(closed);EXPECT_FALSE(pending);pending={};closed={};
}

TEST(ResourceLifecycle, NestedDeleteReceiptSurvivesOnlyItsOriginalAppendSuspensionRoundtrip) {
  Fixture f;const ResourceRegistrationRequest request{f.archive,Kind::Image,0};auto registration=f.catalog.prepareIssue(std::span(&request,1));
  const auto image=registration.resources()[0];auto append=f.catalog.retainAppendAuthority(registration,"Original image append");
  ASSERT_TRUE(f.catalog.exchange(registration));registration={};
  auto deletion=retire(f.catalog,image);{auto undo=f.catalog.prepareChange(deletion,Phase::Active);ASSERT_TRUE(f.catalog.exchange(undo));}
  auto stale=f.catalog.prepareChange(deletion,Phase::Retired);
  const auto before=f.catalog.revision();
  {auto cut=f.catalog.prepareAppendChange(append,Phase::Retired,{},ResourceRetirementPolicy::InvalidatePreparations);
    EXPECT_FALSE(cut.command());bool result{};size_t allocations{};
    {material_publication_test::AllocationGuard deny(true);result=f.catalog.exchange(cut);allocations=deny.allocations();}
    EXPECT_TRUE(result);EXPECT_EQ(allocations,0u);}
  EXPECT_FALSE(f.catalog.active(image));EXPECT_EQ(append.phase(),Phase::Retired);EXPECT_EQ(f.catalog.revision(),before+1);
  EXPECT_THROW(f.catalog.prepareChange(deletion,Phase::Retired),std::invalid_argument);
  {auto restore=f.catalog.prepareAppendChange(append,Phase::Active);ASSERT_TRUE(f.catalog.exchange(restore));}
  EXPECT_TRUE(f.catalog.active(image));EXPECT_EQ(append.phase(),Phase::Active);EXPECT_EQ(f.catalog.revision(),before+2);
  bool oldReady{},oldPublished{};size_t attempts{};
  {material_publication_test::AllocationGuard deny(true);oldReady=f.catalog.canExchange(stale);oldPublished=f.catalog.exchange(stale);attempts=deny.allocations();}
  EXPECT_FALSE(oldReady);EXPECT_FALSE(oldPublished);EXPECT_EQ(attempts,0u);stale={};
  auto redo=f.catalog.prepareChange(deletion,Phase::Retired);ASSERT_TRUE(f.catalog.exchange(redo));
  EXPECT_FALSE(f.catalog.active(image));EXPECT_TRUE(f.catalog.active(f.first));
}

TEST(ResourceLifecycle, OrdinaryOtherReceiptCyclesStillInvalidateDeleteAcrossAppendRoundtrips) {
  Fixture f;const ResourceRegistrationRequest request{f.archive,Kind::Image,0};auto registration=f.catalog.prepareIssue(std::span(&request,1));
  const auto image=registration.resources()[0];auto append=f.catalog.retainAppendAuthority(registration,"Original append");
  ASSERT_TRUE(f.catalog.exchange(registration));registration={};
  auto old=retire(f.catalog,image);{auto undo=f.catalog.prepareChange(old,Phase::Active);ASSERT_TRUE(f.catalog.exchange(undo));}
  auto newer=retire(f.catalog,image);{auto undo=f.catalog.prepareChange(newer,Phase::Active);ASSERT_TRUE(f.catalog.exchange(undo));}
  {auto cut=f.catalog.prepareAppendChange(append,Phase::Retired);ASSERT_TRUE(f.catalog.exchange(cut));}
  {auto restore=f.catalog.prepareAppendChange(append,Phase::Active);ASSERT_TRUE(f.catalog.exchange(restore));}
  EXPECT_THROW(f.catalog.prepareChange(old,Phase::Retired),std::invalid_argument);
  auto valid=f.catalog.prepareChange(newer,Phase::Retired);ASSERT_TRUE(f.catalog.exchange(valid));
  EXPECT_THROW(f.catalog.prepareAppendChange(append,Phase::Retired),std::invalid_argument);
  EXPECT_THROW(f.catalog.prepareAppendChange(append,Phase::Active),std::invalid_argument);
}

TEST(ResourceLifecycle, AppendSuspendRequiresEveryOriginalRowAndExactOutgoingDependencyOwners) {
  Fixture f;const std::array requests{ResourceRegistrationRequest{f.archive,Kind::Image,0},ResourceRegistrationRequest{f.archive,Kind::Sampler,0}};
  auto registration=f.catalog.prepareIssue(requests);const auto image=registration.resources()[0],sampler=registration.resources()[1];
  auto append=f.catalog.retainAppendAuthority(registration,"Original complete resource append");ASSERT_TRUE(f.catalog.exchange(registration));registration={};
  auto first=f.catalog.pin(one(image),Pins::LiveDependency,"Original outgoing image edge");
  auto second=f.catalog.pin(one(sampler),Pins::LiveDependency,"Original outgoing sampler edge");
  const ResourcePin* oneOwner[]{&first};EXPECT_THROW(f.catalog.prepareAppendChange(append,Phase::Retired,oneOwner),std::invalid_argument);
  const ResourcePin* both[]{&first,&second};auto cut=f.catalog.prepareAppendChange(append,Phase::Retired,both);
  auto residual=f.catalog.pin(one(image),Pins::LiveDependency,"Late external owner");EXPECT_FALSE(f.catalog.canExchange(cut));residual.reset();
  ASSERT_TRUE(f.catalog.exchange(cut));EXPECT_FALSE(first);EXPECT_FALSE(second);cut={};
  EXPECT_FALSE(f.catalog.active(image));EXPECT_FALSE(f.catalog.active(sampler));
  auto restore=f.catalog.prepareAppendChange(append,Phase::Active);const std::array targets{image,sampler,f.first};
  auto successor=f.catalog.stagePin(restore,targets,Pins::LiveDependency,"Restored owned and existing dependencies");
  EXPECT_TRUE(successor.pending());ASSERT_TRUE(f.catalog.exchange(restore));EXPECT_TRUE(successor.active());EXPECT_TRUE(successor.resourcesActive());
  EXPECT_TRUE(f.catalog.active(image));EXPECT_TRUE(f.catalog.active(sampler));EXPECT_TRUE(f.catalog.active(f.first));
}

TEST(ResourceLifecycle, AppendLateReleaseAndAbandonedSuccessorDoNotReleaseCurrentOwners) {
  Fixture f;const ResourceRegistrationRequest request{f.archive,Kind::Image,0};auto registration=f.catalog.prepareIssue(std::span(&request,1));
  const auto image=registration.resources()[0];auto append=f.catalog.retainAppendAuthority(registration,"Original append");
  ASSERT_TRUE(f.catalog.exchange(registration));registration={};
  auto unrelated=f.catalog.pin(one(f.first),Pins::LiveDependency,"Old independent material edge");
  auto cut=f.catalog.prepareAppendChange(append,Phase::Retired);const ResourcePin* outgoing[]{&unrelated};
  f.catalog.stageDependencyRelease(cut,outgoing);auto next=f.catalog.stagePin(cut,one(f.second),Pins::LiveDependency,"Abandoned successor");
  cut={};EXPECT_TRUE(unrelated.active());EXPECT_FALSE(next);EXPECT_TRUE(f.catalog.active(image));
  auto retry=f.catalog.prepareAppendChange(append,Phase::Retired);f.catalog.stageDependencyRelease(retry,outgoing);
  auto successor=f.catalog.stagePin(retry,one(f.second),Pins::LiveDependency,"Replacement successor");
  bool published{};size_t allocations{};{material_publication_test::AllocationGuard deny(true);published=f.catalog.exchange(retry);allocations=deny.allocations();}
  EXPECT_TRUE(published);EXPECT_EQ(allocations,0u);EXPECT_FALSE(unrelated);EXPECT_TRUE(successor.active());
}

TEST(ResourceLifecycle, AppendAuthorityAndTransitionAllocationFailuresLeaveRegistrationAndEndpointsExact) {
  size_t creationAllocations{};{
    Fixture f;const ResourceRegistrationRequest request{f.archive,Kind::Image,0};auto registration=f.catalog.prepareIssue(std::span(&request,1));
    material_publication_test::AllocationGuard count(false);auto append=f.catalog.retainAppendAuthority(registration,"Original append");count.disable();creationAllocations=count.allocations();
  }
  ASSERT_GT(creationAllocations,0u);ASSERT_LT(creationAllocations,64u);
  for(size_t allowance=0;allowance<creationAllocations;++allowance) {
    Fixture f;const ResourceRegistrationRequest request{f.archive,Kind::Image,0};auto registration=f.catalog.prepareIssue(std::span(&request,1));
    const auto revision=f.catalog.revision(),bytes=f.catalog.ownedBytes();bool denied=false;
    {material_publication_test::AllocationGuard guard(false,allowance);try {(void)f.catalog.retainAppendAuthority(registration,"Original append");} catch(const std::bad_alloc&) {denied=true;}}
    EXPECT_TRUE(denied);EXPECT_EQ(f.catalog.revision(),revision);EXPECT_EQ(f.catalog.ownedBytes(),bytes);EXPECT_TRUE(f.catalog.canExchange(registration));
    auto retry=f.catalog.retainAppendAuthority(registration,"Retry original append");EXPECT_TRUE(retry);ASSERT_TRUE(f.catalog.exchange(registration));
  }
  size_t transitionAllocations{};{
    Fixture f;const ResourceRegistrationRequest request{f.archive,Kind::Image,0};auto registration=f.catalog.prepareIssue(std::span(&request,1));
    auto append=f.catalog.retainAppendAuthority(registration,"Original append");ASSERT_TRUE(f.catalog.exchange(registration));registration={};
    material_publication_test::AllocationGuard count(false);auto cut=f.catalog.prepareAppendChange(append,Phase::Retired);count.disable();transitionAllocations=count.allocations();
  }
  ASSERT_GT(transitionAllocations,0u);ASSERT_LT(transitionAllocations,64u);
  for(size_t allowance=0;allowance<transitionAllocations;++allowance) {
    Fixture f;const ResourceRegistrationRequest request{f.archive,Kind::Image,0};auto registration=f.catalog.prepareIssue(std::span(&request,1));
    const auto image=registration.resources()[0];auto append=f.catalog.retainAppendAuthority(registration,"Original append");
    ASSERT_TRUE(f.catalog.exchange(registration));registration={};const auto revision=f.catalog.revision(),bytes=f.catalog.ownedBytes();bool denied=false;
    {material_publication_test::AllocationGuard guard(false,allowance);try {(void)f.catalog.prepareAppendChange(append,Phase::Retired);} catch(const std::bad_alloc&) {denied=true;}}
    EXPECT_TRUE(denied);EXPECT_EQ(f.catalog.revision(),revision);EXPECT_EQ(f.catalog.ownedBytes(),bytes);EXPECT_TRUE(f.catalog.active(image));EXPECT_EQ(append.phase(),Phase::Active);
    auto retry=f.catalog.prepareAppendChange(append,Phase::Retired);ASSERT_TRUE(f.catalog.exchange(retry));
  }
}

TEST(ResourceLifecycle, PendingAppendCommandsConsumeAggregateLimitsAndCancellationReleasesCapacity) {
  ResourceLifecycleLimits limits;limits.maxCommands=1;limits.maxCommandReferences=1;ResourceLifecycleCatalog catalog(limits);
  const auto domain=catalog.createDomain(DomainKind::Archive);const ResourceRegistrationRequest request{domain,Kind::Image,0};
  auto registration=catalog.prepareIssue(std::span(&request,1));auto append=catalog.retainAppendAuthority(registration,"Pending append");
  const ResourceRegistrationRequest secondRequest{domain,Kind::Sampler,0};auto second=catalog.prepareIssue(std::span(&secondRequest,1));
  EXPECT_THROW(catalog.retainAppendAuthority(second,"No command capacity"),std::length_error);
  registration={};EXPECT_FALSE(append);auto current=catalog.retainAppendAuthority(second,"Capacity released");
  ASSERT_TRUE(catalog.exchange(second));second={};const auto handle=current.resources()[0];
  {auto cut=catalog.prepareAppendChange(current,Phase::Retired);ASSERT_TRUE(catalog.exchange(cut));}
  EXPECT_FALSE(catalog.reclaimable(handle));current={};EXPECT_TRUE(catalog.reclaimable(handle));
}

} // namespace
