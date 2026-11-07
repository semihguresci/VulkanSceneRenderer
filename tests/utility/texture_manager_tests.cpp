#include <Container/utility/TextureManager.h>
#include "../renderer/scene/material_publication_allocations.h"

#include <optional>
#include <array>
#include <stdexcept>
#include <string>

namespace {
using container::material::TextureManager;
using container::material::TextureResource;
using namespace container::material;

TextureResource resource(std::string name,std::string content) {
  TextureResource result;result.name=std::move(name);result.contentKey=std::move(content);
  result.width=7;result.height=11;result.mipLevels=1;result.format=VK_FORMAT_R8G8B8A8_UNORM;
  return result;
}

TextureManager tableWithRetiredMiddle() {
  TextureManager result;
  (void)result.registerTexture(resource("first-long-logical-name-for-allocation-boundaries","first-long-content-for-allocation-boundaries"));
  (void)result.registerTexture(resource("middle-long-logical-name-for-allocation-boundaries","shared-content-for-explicit-aliases"));
  (void)result.registerTexture(resource("last-long-logical-name-for-allocation-boundaries","shared-content-for-explicit-aliases"));
  (void)result.registerTexture(resource("tail-long-logical-name-for-allocation-boundaries","tail-long-content-for-allocation-boundaries"));
  if(!result.retireTexture(1)) throw std::logic_error("test table did not retire its middle row");
  return result;
}

void expectOriginal(const TextureManager& table,uint64_t revision) {
  EXPECT_EQ(table.textureCount(),4u);EXPECT_EQ(table.activeTextureCount(),3u);EXPECT_EQ(table.revision(),revision);
  EXPECT_TRUE(table.isTextureActive(0));EXPECT_FALSE(table.isTextureActive(1));EXPECT_TRUE(table.isTextureActive(2));EXPECT_TRUE(table.isTextureActive(3));
  EXPECT_FALSE(table.findTextureIndex("middle-long-logical-name-for-allocation-boundaries"));
  EXPECT_EQ(table.findTextureImage("shared-content-for-explicit-aliases"),nullptr);
  EXPECT_EQ(table.findTextureIndex("last-long-logical-name-for-allocation-boundaries"),std::optional<uint32_t>(2));
}
} // namespace

TEST(TextureManager, RevisionChangesOnlyForPublishedNewRowsAndRetirement) {
  TextureManager table;EXPECT_EQ(table.revision(),0u);
  const auto first=resource("first","shared");ASSERT_EQ(table.registerTexture(first),0u);EXPECT_EQ(table.revision(),1u);
  EXPECT_EQ(table.registerTexture(first),0u);EXPECT_EQ(table.revision(),1u);
  EXPECT_FALSE(table.retireTexture(9));EXPECT_EQ(table.revision(),1u);
  ASSERT_TRUE(table.retireTexture(0));EXPECT_EQ(table.revision(),2u);
  EXPECT_FALSE(table.retireTexture(0));EXPECT_EQ(table.revision(),2u);
  EXPECT_EQ(table.registerTexture(first),1u);EXPECT_EQ(table.revision(),3u);
  EXPECT_EQ(table.textureCount(),2u);EXPECT_EQ(table.activeTextureCount(),1u);
}

TEST(TextureManager, ExactPrefixPreservesTombstonesIndicesAndErasedContentAlias) {
  const auto table=tableWithRetiredMiddle();const auto revision=table.revision();
  auto prefix=table.copyPrefix(3);
  EXPECT_EQ(prefix.textureCount(),3u);EXPECT_EQ(prefix.activeTextureCount(),2u);EXPECT_EQ(prefix.revision(),revision);
  EXPECT_EQ(prefix.getTexture(1),nullptr);EXPECT_FALSE(prefix.findTextureIndex("middle-long-logical-name-for-allocation-boundaries"));
  EXPECT_EQ(prefix.findTextureImage("shared-content-for-explicit-aliases"),nullptr);
  EXPECT_EQ(prefix.findTextureIndex("last-long-logical-name-for-allocation-boundaries"),std::optional<uint32_t>(2));
  ASSERT_NE(prefix.getTexture(2),nullptr);EXPECT_EQ(prefix.getTexture(2)->width,7u);EXPECT_EQ(prefix.getTexture(2)->height,11u);
  EXPECT_FALSE(prefix.findTextureIndex("tail-long-logical-name-for-allocation-boundaries"));
  EXPECT_EQ(prefix.findTextureImage("tail-long-content-for-allocation-boundaries"),nullptr);
  EXPECT_EQ(prefix.registerTexture(resource("new-tail","new-tail-content")),3u);
  EXPECT_EQ(prefix.revision(),revision+1);expectOriginal(table,revision);
}

TEST(TextureManager, EmptyAndAllDeadPrefixesRemainStableSnapshots) {
  auto table=tableWithRetiredMiddle();const auto revision=table.revision();
  const auto empty=table.copyPrefix(0);EXPECT_EQ(empty.textureCount(),0u);EXPECT_EQ(empty.activeTextureCount(),0u);EXPECT_EQ(empty.revision(),revision);
  ASSERT_TRUE(table.retireTexture(0));const auto dead=table.copyPrefix(2);
  EXPECT_EQ(dead.textureCount(),2u);EXPECT_EQ(dead.activeTextureCount(),0u);EXPECT_EQ(dead.revision(),table.revision());
  EXPECT_EQ(dead.getTexture(0),nullptr);EXPECT_EQ(dead.getTexture(1),nullptr);
  EXPECT_FALSE(dead.findTextureIndex("first-long-logical-name-for-allocation-boundaries"));
  EXPECT_EQ(dead.findTextureImage("first-long-content-for-allocation-boundaries"),nullptr);
  EXPECT_THROW((void)table.copyPrefix(5),std::invalid_argument);
}

TEST(TextureManager, RemovingCanonicalContentTailDoesNotRetargetAnOlderSurvivingAlias) {
  auto table=tableWithRetiredMiddle();
  ASSERT_EQ(table.findTextureImage("shared-content-for-explicit-aliases"),nullptr);
  const auto canonical=table.registerTexture(resource("new-canonical-tail","shared-content-for-explicit-aliases"));
  ASSERT_EQ(canonical,4u);ASSERT_EQ(table.findTextureImage("shared-content-for-explicit-aliases"),table.getTexture(canonical));
  const auto prefix=table.copyPrefix(4);
  EXPECT_TRUE(prefix.isTextureActive(2));EXPECT_EQ(prefix.findTextureImage("shared-content-for-explicit-aliases"),nullptr);
  EXPECT_EQ(prefix.findTextureIndex("last-long-logical-name-for-allocation-boundaries"),std::optional<uint32_t>(2));
  EXPECT_FALSE(prefix.findTextureIndex("new-canonical-tail"));
  EXPECT_EQ(table.findTextureImage("shared-content-for-explicit-aliases"),table.getTexture(canonical));
}

TEST(TextureManager, RetirementAndTableExchangeAllocateNothingAndCarryRevision) {
  auto table=tableWithRetiredMiddle();auto snapshot=table.copyPrefix(3);const auto revision=table.revision();
  bool retired{};size_t attempts{};
  {material_publication_test::AllocationGuard guard(true);retired=table.retireTexture(2);using std::swap;swap(table,snapshot);attempts=guard.allocations();}
  EXPECT_TRUE(retired);EXPECT_EQ(attempts,0u);EXPECT_EQ(table.revision(),revision);EXPECT_EQ(snapshot.revision(),revision+1);
  EXPECT_EQ(table.textureCount(),3u);EXPECT_TRUE(table.isTextureActive(2));EXPECT_FALSE(snapshot.isTextureActive(2));
}

TEST(TextureManager, DeniedPrefixAllocationsNeverModifyOriginalRowsOrAliasCaches) {
  const auto table=tableWithRetiredMiddle();const auto revision=table.revision();
  bool completed{};size_t failures{};
  for(size_t allowance=0;allowance<128 && !completed;++allowance) {
    std::optional<TextureManager> prefix;
    try {material_publication_test::AllocationGuard guard(false,allowance);prefix.emplace(table.copyPrefix(3));}
    catch(const std::bad_alloc&) {++failures;}
    expectOriginal(table,revision);
    if(prefix) {EXPECT_EQ(prefix->textureCount(),3u);EXPECT_FALSE(prefix->isTextureActive(1));EXPECT_EQ(prefix->findTextureImage("shared-content-for-explicit-aliases"),nullptr);completed=true;}
  }
  EXPECT_GT(failures,3u);EXPECT_TRUE(completed);
}

TEST(TextureManager, OriginalReceiptRestoresSelectedStableRowsAndOnlyTheirOriginalAliases) {
  TextureManager table;const auto original=resource("original-runtime-long-name","shared-image-content");
  ASSERT_EQ(table.registerTexture(original),0u);ASSERT_EQ(table.registerTexture(resource("unselected-alias","shared-image-content")),1u);
  ASSERT_EQ(table.registerTexture(resource("unrelated-tail","unrelated-content")),2u);const auto revision=table.revision();
  const std::array slots{uint32_t(0)};auto prepared=PrepareRuntimeTextureRetirement(table,slots);const auto command=prepared.command();
  EXPECT_FALSE(command.published());EXPECT_TRUE(table.isTextureActive(0));EXPECT_FALSE(prepared.textures().isTextureActive(0));
  EXPECT_EQ(prepared.textures().textureCount(),3u);EXPECT_EQ(prepared.textures().findTextureImage("shared-image-content"),nullptr);
  bool retired{},restored{};size_t attempts{};
  {material_publication_test::AllocationGuard deny(true);retired=ExchangeRuntimeTextureRetirement(prepared,table);attempts=deny.allocations();}
  ASSERT_TRUE(retired);EXPECT_EQ(attempts,0u);EXPECT_TRUE(command.retired());EXPECT_EQ(table.revision(),revision+1);EXPECT_TRUE(table.isTextureActive(1));
  auto restore=PrepareRuntimeTextureRetirementReplay(table,command,RuntimeTextureRetirementTarget::Active);
  {material_publication_test::AllocationGuard deny(true);restored=ExchangeRuntimeTextureRetirement(restore,table);attempts=deny.allocations();}
  ASSERT_TRUE(restored);EXPECT_EQ(attempts,0u);EXPECT_FALSE(command.retired());EXPECT_EQ(table.revision(),revision+2);
  ASSERT_NE(table.getTexture(0),nullptr);EXPECT_EQ(table.getTexture(0)->name,original.name);EXPECT_EQ(table.getTexture(0)->width,original.width);
  EXPECT_EQ(table.findTextureImage("shared-image-content"),table.getTexture(0));EXPECT_EQ(table.findTextureIndex(original.name),std::optional<uint32_t>(0));
  EXPECT_EQ(table.findTextureIndex("unselected-alias"),std::optional<uint32_t>(1));
  auto redo=PrepareRuntimeTextureRetirementReplay(table,command,RuntimeTextureRetirementTarget::Retired);ASSERT_TRUE(ExchangeRuntimeTextureRetirement(redo,table));
  EXPECT_EQ(table.revision(),revision+3);EXPECT_EQ(table.textureCount(),3u);EXPECT_FALSE(table.isTextureActive(0));
}

TEST(TextureManager, NoncanonicalAliasRestoreDoesNotInventAnErasedContentCacheCell) {
  auto table=tableWithRetiredMiddle();const auto revision=table.revision();const std::array selected{uint32_t(2)};
  auto retire=PrepareRuntimeTextureRetirement(table,selected);const auto command=retire.command();ASSERT_TRUE(ExchangeRuntimeTextureRetirement(retire,table));
  auto restore=PrepareRuntimeTextureRetirementReplay(table,command,RuntimeTextureRetirementTarget::Active);ASSERT_TRUE(ExchangeRuntimeTextureRetirement(restore,table));
  expectOriginal(table,revision+2);EXPECT_EQ(table.findTextureImage("shared-content-for-explicit-aliases"),nullptr);
}

TEST(TextureManager, CompatibleCopiedBranchCanReplayButForeignAndCanceledReceiptsCannot) {
  TextureManager table;const auto row=resource("row","content");(void)table.registerTexture(row);const std::array selected{uint32_t(0)};
  RuntimeTextureRetirementCommand canceled;{auto pending=PrepareRuntimeTextureRetirement(table,selected);canceled=pending.command();}
  EXPECT_THROW((void)PrepareRuntimeTextureRetirementReplay(table,canceled,RuntimeTextureRetirementTarget::Active),std::invalid_argument);
  auto retire=PrepareRuntimeTextureRetirement(table,selected);const auto command=retire.command();ASSERT_TRUE(ExchangeRuntimeTextureRetirement(retire,table));
  TextureManager foreign;(void)foreign.registerTexture(row);ASSERT_TRUE(foreign.retireTexture(0));
  EXPECT_THROW((void)PrepareRuntimeTextureRetirementReplay(foreign,command,RuntimeTextureRetirementTarget::Active),std::invalid_argument);
  auto clone=table;auto restore=PrepareRuntimeTextureRetirementReplay(clone,command,RuntimeTextureRetirementTarget::Active);
  ASSERT_TRUE(ExchangeRuntimeTextureRetirement(restore,clone));EXPECT_TRUE(clone.isTextureActive(0));EXPECT_FALSE(table.isTextureActive(0));
  EXPECT_THROW((void)PrepareRuntimeTextureRetirementReplay(table,command,RuntimeTextureRetirementTarget::Retired),std::invalid_argument);
}

TEST(TextureManager, RestoreRejectsNewNameAndContentAliasOwnersWithoutStealingTheirCells) {
  for(const bool nameCollision:{false,true}) {
    TextureManager table;(void)table.registerTexture(resource("original-name","original-content"));const std::array selected{uint32_t(0)};
    auto retire=PrepareRuntimeTextureRetirement(table,selected);const auto command=retire.command();ASSERT_TRUE(ExchangeRuntimeTextureRetirement(retire,table));
    const auto newer=table.registerTexture(resource(nameCollision?"original-name":"new-name",nameCollision?"new-content":"original-content"));
    const auto revision=table.revision();EXPECT_EQ(newer,1u);
    EXPECT_THROW((void)PrepareRuntimeTextureRetirementReplay(table,command,RuntimeTextureRetirementTarget::Active),std::invalid_argument);
    EXPECT_EQ(table.revision(),revision);EXPECT_FALSE(table.isTextureActive(0));EXPECT_TRUE(table.isTextureActive(newer));
    if(nameCollision) EXPECT_EQ(table.findTextureIndex("original-name"),std::optional<uint32_t>(newer));
    else EXPECT_EQ(table.findTextureImage("original-content"),table.getTexture(newer));
  }
}

TEST(TextureManager, SelectedPayloadAndPhaseVersionsCannotBeReboundByTrimOrAnotherReceipt) {
  TextureManager table;(void)table.registerTexture(resource("original","bytes"));const std::array selected{uint32_t(0)};
  auto retire=PrepareRuntimeTextureRetirement(table,selected);const auto command=retire.command();ASSERT_TRUE(ExchangeRuntimeTextureRetirement(retire,table));
  auto branch=table.copyPrefix(0);EXPECT_EQ(branch.registerTexture(resource("original","bytes")),0u);
  EXPECT_THROW((void)PrepareRuntimeTextureRetirementReplay(branch,command,RuntimeTextureRetirementTarget::Active),std::invalid_argument);
  auto restore=PrepareRuntimeTextureRetirementReplay(table,command,RuntimeTextureRetirementTarget::Active);ASSERT_TRUE(ExchangeRuntimeTextureRetirement(restore,table));
  auto second=PrepareRuntimeTextureRetirement(table,selected);const auto other=second.command();ASSERT_TRUE(ExchangeRuntimeTextureRetirement(second,table));
  auto restoreSecond=PrepareRuntimeTextureRetirementReplay(table,other,RuntimeTextureRetirementTarget::Active);ASSERT_TRUE(ExchangeRuntimeTextureRetirement(restoreSecond,table));
  EXPECT_THROW((void)PrepareRuntimeTextureRetirementReplay(table,command,RuntimeTextureRetirementTarget::Retired),std::invalid_argument);
}

TEST(TextureManager, SameRevisionForkCannotOverwriteUnselectedRowsFromAnotherBranch) {
  TextureManager base;(void)base.registerTexture(resource("selected","selected-content"));
  auto table=base;auto fork=base;(void)table.registerTexture(resource("left","left-content"));(void)fork.registerTexture(resource("right","right-content"));
  ASSERT_EQ(table.revision(),fork.revision());const std::array selected{uint32_t(0)};auto candidate=PrepareRuntimeTextureRetirement(table,selected);
  using std::swap;swap(table,fork);bool ready{},published{};size_t attempts{};
  {material_publication_test::AllocationGuard deny(true);ready=CanExchangeRuntimeTextureRetirement(candidate,table);published=ExchangeRuntimeTextureRetirement(candidate,table);attempts=deny.allocations();}
  EXPECT_FALSE(ready);EXPECT_FALSE(published);EXPECT_EQ(attempts,0u);ASSERT_NE(table.getTexture(1),nullptr);EXPECT_EQ(table.getTexture(1)->name,"right");
}

TEST(TextureManager, UnrelatedCurrentAppendsSurviveFreshReceiptReplayAndAdvanceRevision) {
  TextureManager table;(void)table.registerTexture(resource("selected","selected-content"));const std::array selected{uint32_t(0)};
  auto retire=PrepareRuntimeTextureRetirement(table,selected);const auto command=retire.command();ASSERT_TRUE(ExchangeRuntimeTextureRetirement(retire,table));
  (void)table.registerTexture(resource("new-tail","new-content"));const auto revision=table.revision();
  auto restore=PrepareRuntimeTextureRetirementReplay(table,command,RuntimeTextureRetirementTarget::Active);ASSERT_TRUE(ExchangeRuntimeTextureRetirement(restore,table));
  EXPECT_EQ(table.revision(),revision+1);EXPECT_EQ(table.textureCount(),2u);EXPECT_EQ(table.findTextureIndex("new-tail"),std::optional<uint32_t>(1));
}

TEST(TextureManager, RuntimeReceiptSelectionsAndStorageAreBounded) {
  TextureManager table;(void)table.registerTexture(resource("selected","selected-content"));const std::array selected{uint32_t(0)};
  EXPECT_THROW((void)PrepareRuntimeTextureRetirement(table,{}),std::invalid_argument);
  const std::array duplicates{uint32_t(0),uint32_t(0)};EXPECT_THROW((void)PrepareRuntimeTextureRetirement(table,duplicates),std::invalid_argument);
  const std::array missing{uint32_t(8)};EXPECT_THROW((void)PrepareRuntimeTextureRetirement(table,missing),std::invalid_argument);
  RuntimeTextureRetirementLimits limits;limits.maxOwnedBytes=32;EXPECT_THROW((void)PrepareRuntimeTextureRetirement(table,selected,limits),std::length_error);
  limits={};limits.maxWork=1;EXPECT_THROW((void)PrepareRuntimeTextureRetirement(table,selected,limits),std::length_error);
  auto retire=PrepareRuntimeTextureRetirement(table,selected);const auto command=retire.command();ASSERT_TRUE(ExchangeRuntimeTextureRetirement(retire,table));
  limits={};limits.maxOwnedBytes=command.ownedBytes();EXPECT_THROW((void)PrepareRuntimeTextureRetirementReplay(table,command,RuntimeTextureRetirementTarget::Active,limits),std::length_error);
}

TEST(TextureManager, EveryDeniedRetireOrRestoreAllocationLeavesCurrentTableAndReceiptUntouched) {
  for(const bool restoring:{false,true}) {
    TextureManager table;(void)table.registerTexture(resource("selected-name-with-heap-storage","selected-content-with-heap-storage"));
    const std::array selected{uint32_t(0)};RuntimeTextureRetirementCommand command;
    if(restoring) {auto retire=PrepareRuntimeTextureRetirement(table,selected);command=retire.command();ASSERT_TRUE(ExchangeRuntimeTextureRetirement(retire,table));}
    const auto revision=table.revision();size_t allocations{};
    {material_publication_test::AllocationGuard count(false);auto probe=restoring?PrepareRuntimeTextureRetirementReplay(table,command,RuntimeTextureRetirementTarget::Active):PrepareRuntimeTextureRetirement(table,selected);allocations=count.allocations();}
    ASSERT_GT(allocations,0u);ASSERT_LT(allocations,512u);
    for(size_t allowance=0;allowance<allocations;++allowance) {
      bool denied{};{material_publication_test::AllocationGuard deny(false,allowance);
        try {auto candidate=restoring?PrepareRuntimeTextureRetirementReplay(table,command,RuntimeTextureRetirementTarget::Active):PrepareRuntimeTextureRetirement(table,selected);}catch(const std::bad_alloc&) {denied=true;}}
      EXPECT_TRUE(denied);EXPECT_EQ(table.revision(),revision);EXPECT_EQ(table.isTextureActive(0),!restoring);EXPECT_EQ(command.retired(),restoring);
    }
  }
}

TEST(TextureManager, ReceiptOwnsOnlySelectedPayloadAndAliasesAfterPreparationIsReleased) {
  TextureManager table;(void)table.registerTexture(resource("selected","selected-content"));
  (void)table.registerTexture(resource(std::string(300'000,'n'),std::string(300'000,'c')));
  const std::array selected{uint32_t(0)};RuntimeTextureRetirementCommand command;
  {auto retire=PrepareRuntimeTextureRetirement(table,selected);command=retire.command();EXPECT_LT(command.ownedBytes(),16'384u);
    EXPECT_GT(retire.ownedBytes(),600'000u);ASSERT_TRUE(ExchangeRuntimeTextureRetirement(retire,table));}
  ASSERT_TRUE(command.published());ASSERT_TRUE(command.retired());EXPECT_EQ(command.slots().size(),1u);
  auto restore=PrepareRuntimeTextureRetirementReplay(table,command,RuntimeTextureRetirementTarget::Active);ASSERT_TRUE(ExchangeRuntimeTextureRetirement(restore,table));
  ASSERT_NE(table.getTexture(1),nullptr);EXPECT_EQ(table.getTexture(1)->name.size(),300'000u);EXPECT_EQ(table.findTextureIndex("selected"),std::optional<uint32_t>(0));
}

TEST(TextureManager, ExplicitMultipleSlotsPublishOneMonotonicRevisionAndKeepEveryStableAddress) {
  TextureManager table;for(const auto* name:{"first","middle","last"}) (void)table.registerTexture(resource(name,name));
  const auto revision=table.revision();const std::array selected{uint32_t(2),uint32_t(0)};
  auto retire=PrepareRuntimeTextureRetirement(table,selected);const auto command=retire.command();EXPECT_EQ(command.slots()[0],0u);EXPECT_EQ(command.slots()[1],2u);
  ASSERT_TRUE(ExchangeRuntimeTextureRetirement(retire,table));EXPECT_EQ(table.revision(),revision+1);EXPECT_EQ(table.textureCount(),3u);EXPECT_EQ(table.activeTextureCount(),1u);
  auto restore=PrepareRuntimeTextureRetirementReplay(table,command,RuntimeTextureRetirementTarget::Active);ASSERT_TRUE(ExchangeRuntimeTextureRetirement(restore,table));
  EXPECT_EQ(table.revision(),revision+2);EXPECT_EQ(table.activeTextureCount(),3u);EXPECT_EQ(table.findTextureIndex("last"),std::optional<uint32_t>(2));
}

TEST(TextureManager, OriginalAppendCutAndRestorePreserveEarlierDeleteReceiptIdentityAndCurrentPhaseVersion) {
  TextureManager table;(void)table.registerTexture(resource("prefix","prefix-content"));auto appended=table;
  (void)appended.registerTexture(resource("import-A1","A1-content"));auto append=PrepareRuntimeTextureAppend(table,appended);const auto import=append.command();
  ASSERT_TRUE(ExchangeRuntimeTextureAppend(append,table));const std::array selected{uint32_t(1)};
  auto deletion=PrepareRuntimeTextureRetirement(table,selected);const auto originalDelete=deletion.command();ASSERT_TRUE(ExchangeRuntimeTextureRetirement(deletion,table));
  auto undoDelete=PrepareRuntimeTextureRetirementReplay(table,originalDelete,RuntimeTextureRetirementTarget::Active);ASSERT_TRUE(ExchangeRuntimeTextureRetirement(undoDelete,table));
  const auto revision=table.revision();auto cut=PrepareRuntimeTextureAppendReplay(table,import,false);bool cutPublished{},restored{};size_t attempts{};
  {material_publication_test::AllocationGuard deny(true);cutPublished=ExchangeRuntimeTextureAppend(cut,table);attempts=deny.allocations();}
  ASSERT_TRUE(cutPublished);EXPECT_EQ(attempts,0u);EXPECT_EQ(table.textureCount(),1u);
  auto restore=PrepareRuntimeTextureAppendReplay(table,import,true);
  {material_publication_test::AllocationGuard deny(true);restored=ExchangeRuntimeTextureAppend(restore,table);attempts=deny.allocations();}
  ASSERT_TRUE(restored);EXPECT_EQ(attempts,0u);EXPECT_EQ(table.revision(),revision+2);EXPECT_EQ(table.findTextureIndex("import-A1"),std::optional<uint32_t>(1));
  // Redo the ORIGINAL Delete after UndoImport/RedoImport. Fresh registration
  // would have silently replaced its incarnation or reset its phase version.
  auto redoDelete=PrepareRuntimeTextureRetirementReplay(table,originalDelete,RuntimeTextureRetirementTarget::Retired);
  ASSERT_TRUE(ExchangeRuntimeTextureRetirement(redoDelete,table));EXPECT_FALSE(table.isTextureActive(1));EXPECT_LT(import.ownedBytes(),16'384u);
}

TEST(TextureManager, OriginalAppendRejectsCanceledForeignAndCutReusedNamespaceRestoration) {
  TextureManager table;(void)table.registerTexture(resource("prefix","prefix-content"));auto appended=table;(void)appended.registerTexture(resource("tail","tail-content"));
  RuntimeTextureAppendCommand abandoned;{auto candidate=PrepareRuntimeTextureAppend(table,appended);abandoned=candidate.command();}
  EXPECT_THROW((void)PrepareRuntimeTextureAppendReplay(table,abandoned,true),std::invalid_argument);
  auto candidate=PrepareRuntimeTextureAppend(table,appended);const auto command=candidate.command();ASSERT_TRUE(ExchangeRuntimeTextureAppend(candidate,table));
  auto cut=PrepareRuntimeTextureAppendReplay(table,command,false);ASSERT_TRUE(ExchangeRuntimeTextureAppend(cut,table));
  TextureManager foreign;(void)foreign.registerTexture(resource("prefix","prefix-content"));
  EXPECT_THROW((void)PrepareRuntimeTextureAppendReplay(foreign,command,true),std::invalid_argument);
  (void)table.registerTexture(resource("new-branch","new-branch-content"));table=table.copyPrefix(1);
  EXPECT_THROW((void)PrepareRuntimeTextureAppendReplay(table,command,true),std::invalid_argument);
  EXPECT_EQ(table.textureCount(),1u);EXPECT_FALSE(table.findTextureIndex("tail"));
}

TEST(TextureManager, AppendReceiptCompatibleCloneRestoresOnlyItsTailAndRejectsWrongPhase) {
  TextureManager table;auto appended=table;(void)appended.registerTexture(resource("first-import","first-content"));
  auto candidate=PrepareRuntimeTextureAppend(table,appended);const auto command=candidate.command();ASSERT_TRUE(ExchangeRuntimeTextureAppend(candidate,table));
  EXPECT_THROW((void)PrepareRuntimeTextureAppendReplay(table,command,true),std::invalid_argument);
  auto cut=PrepareRuntimeTextureAppendReplay(table,command,false);ASSERT_TRUE(ExchangeRuntimeTextureAppend(cut,table));auto clone=table;
  auto restore=PrepareRuntimeTextureAppendReplay(clone,command,true);ASSERT_TRUE(ExchangeRuntimeTextureAppend(restore,clone));
  EXPECT_EQ(clone.textureCount(),1u);EXPECT_EQ(table.textureCount(),0u);EXPECT_EQ(clone.findTextureIndex("first-import"),std::optional<uint32_t>(0));
}

TEST(TextureManager, OriginalAppendRestoresCurrentNestedChildVacancyAndRejectsReplacementBranch) {
  TextureManager table;(void)table.registerTexture(resource("prefix","prefix-content"));
  const auto publish=[&](PreparedRuntimeTextureAppend& change) {
    bool ready{},published{};size_t attempts{};
    {material_publication_test::AllocationGuard deny(true);ready=CanExchangeRuntimeTextureAppend(change,table);
      published=ExchangeRuntimeTextureAppend(change,table);attempts=deny.allocations();}
    EXPECT_TRUE(ready);EXPECT_EQ(attempts,0u);return published;
  };
  auto appendedA=table;(void)appendedA.registerTexture(resource("A","shared-image"));
  auto addA=PrepareRuntimeTextureAppend(table,appendedA);const auto a=addA.command();ASSERT_TRUE(publish(addA));
  auto appendedB=table;auto bPayload=resource("B-sampler-alias","shared-image");bPayload.samplerIndex=1;
  (void)appendedB.registerTexture(bPayload);auto addB=PrepareRuntimeTextureAppend(table,appendedB);const auto b=addB.command();ASSERT_TRUE(publish(addB));
  auto cutB=PrepareRuntimeTextureAppendReplay(table,b,false);ASSERT_TRUE(publish(cutB));
  auto cutA=PrepareRuntimeTextureAppendReplay(table,a,false);ASSERT_TRUE(publish(cutA));EXPECT_EQ(table.textureCount(),1u);
  auto restoreA=PrepareRuntimeTextureAppendReplay(table,a,true);ASSERT_TRUE(publish(restoreA));EXPECT_EQ(table.textureCount(),2u);
  auto restoreB=PrepareRuntimeTextureAppendReplay(table,b,true);ASSERT_TRUE(publish(restoreB));
  EXPECT_EQ(table.textureCount(),3u);ASSERT_NE(table.getTexture(2),nullptr);EXPECT_EQ(table.getTexture(2)->samplerIndex,1u);
  EXPECT_EQ(table.findTextureImage("shared-image"),table.getTexture(1));
  auto cutBAgain=PrepareRuntimeTextureAppendReplay(table,b,false);ASSERT_TRUE(publish(cutBAgain));
  auto appendedC=table;auto cPayload=resource("C-replacement-branch","shared-image");cPayload.samplerIndex=2;
  (void)appendedC.registerTexture(cPayload);auto addC=PrepareRuntimeTextureAppend(table,appendedC);const auto c=addC.command();ASSERT_TRUE(publish(addC));
  auto cutC=PrepareRuntimeTextureAppendReplay(table,c,false);ASSERT_TRUE(publish(cutC));
  // Equal prefix shape after a DIFFERENT child's cut cannot revive B.
  EXPECT_THROW((void)PrepareRuntimeTextureAppendReplay(table,b,true),std::invalid_argument);
  EXPECT_EQ(table.textureCount(),2u);EXPECT_FALSE(table.findTextureIndex("B-sampler-alias"));
}

TEST(TextureManager, EveryDeniedOriginalAppendCutOrRestoreAllocationLeavesItsLiveNamespaceUntouched) {
  for(int phase=0;phase<3;++phase) {
    TextureManager table;(void)table.registerTexture(resource("prefix-long-storage","prefix-content-long-storage"));auto appended=table;
    (void)appended.registerTexture(resource("tail-long-storage","tail-content-long-storage"));RuntimeTextureAppendCommand command;
    if(phase) {auto initial=PrepareRuntimeTextureAppend(table,appended);command=initial.command();ASSERT_TRUE(ExchangeRuntimeTextureAppend(initial,table));}
    if(phase==2) {auto cut=PrepareRuntimeTextureAppendReplay(table,command,false);ASSERT_TRUE(ExchangeRuntimeTextureAppend(cut,table));}
    const auto revision=table.revision();const auto count=table.textureCount();size_t allocations{};
    {material_publication_test::AllocationGuard record(false);auto probe=phase?PrepareRuntimeTextureAppendReplay(table,command,phase==2):PrepareRuntimeTextureAppend(table,appended);allocations=record.allocations();}
    ASSERT_GT(allocations,0u);ASSERT_LT(allocations,512u);
    for(size_t allowance=0;allowance<allocations;++allowance) {
      bool denied{};{material_publication_test::AllocationGuard fail(false,allowance);
        try {auto pending=phase?PrepareRuntimeTextureAppendReplay(table,command,phase==2):PrepareRuntimeTextureAppend(table,appended);}catch(const std::bad_alloc&) {denied=true;}}
      EXPECT_TRUE(denied);EXPECT_EQ(table.revision(),revision);EXPECT_EQ(table.textureCount(),count);
      EXPECT_EQ(table.findTextureIndex("prefix-long-storage"),std::optional<uint32_t>(0));
    }
  }
}

TEST(TextureManager, DeniedNewRowAllocationsPreserveRevisionAndExistingCacheEntries) {
  const auto original=tableWithRetiredMiddle();const auto revision=original.revision();
  const auto next=resource("new-distinct-long-name-for-allocation-boundaries","new-distinct-long-content-for-allocation-boundaries");
  bool completed{};size_t failures{};
  for(size_t allowance=0;allowance<128 && !completed;++allowance) {
    auto table=original;
    try {material_publication_test::AllocationGuard guard(false,allowance);(void)table.registerTexture(next);completed=true;}
    catch(const std::bad_alloc&) {++failures;}
    if(!completed) {expectOriginal(table,revision);EXPECT_FALSE(table.findTextureIndex(next.name));EXPECT_EQ(table.findTextureImage(next.contentKey),nullptr);}
    else {EXPECT_EQ(table.textureCount(),5u);EXPECT_EQ(table.revision(),revision+1);EXPECT_EQ(table.findTextureIndex(next.name),std::optional<uint32_t>(4));}
  }
  EXPECT_GT(failures,3u);EXPECT_TRUE(completed);
}
