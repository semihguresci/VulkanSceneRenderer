#include <Container/utility/SceneGraph.h>
#include <gtest/gtest.h>

#include "../renderer/scene/material_publication_allocations.h"

#include <glm/gtc/matrix_transform.hpp>
#include <array>
#include <optional>
#include <stdexcept>
#include <vector>

namespace {
using container::scene::SceneGraph;
using material_publication_test::AllocationGuard;

SceneGraph graphFixture() {
  SceneGraph graph;
  const auto parent=graph.createNode(glm::mat4(1),0,false,SceneGraph::kInvalidNode,"Parent");
  const auto child=graph.createNode(glm::mat4(1),0,false,SceneGraph::kInvalidNode,"Camera");
  graph.setParent(child,parent);graph.updateWorldTransforms();
  return graph;
}

TEST(SceneNodeLifetime, DistinctNonzeroSlotsStayPrivateFromEqualPublicPayload) {
  auto graph=graphFixture();const auto first=graph.nodeIncarnation(0),second=graph.nodeIncarnation(1);
  EXPECT_NE(first,0u);EXPECT_NE(second,0u);EXPECT_NE(first,second);
  const auto original=*graph.getNode(1);*graph.getNode(1)=*graph.getNode(0);
  EXPECT_EQ(graph.nodeIncarnation(1),second);*graph.getNode(1)=original;
  EXPECT_EQ(graph.nodeIncarnation(SceneGraph::kInvalidNode),0u);
  EXPECT_EQ(graph.nodeIncarnation(2),0u);
}

TEST(SceneNodeLifetime, CopyAndMoveKeepSlotBirthButAlwaysSeparateGraphIdentity) {
  auto graph=graphFixture();const auto old=graph.identity(),birth=graph.nodeIncarnation(1);
  SceneGraph copied(graph);EXPECT_NE(copied.identity(),old);EXPECT_EQ(copied.nodeIncarnation(1),birth);
  const auto copyId=copied.identity();SceneGraph moved(std::move(copied));
  EXPECT_NE(moved.identity(),copyId);EXPECT_EQ(moved.nodeIncarnation(1),birth);
  EXPECT_EQ(copied.nodeCount(),0u);EXPECT_EQ(copied.nodeIncarnation(1),0u);
}

TEST(SceneNodeLifetime, PlacementGuardAndExchangeAllocateNothingAndKeepBirths) {
  auto graph=graphFixture();const auto identity=graph.identity(),revision=graph.revision();
  const std::array births{graph.nodeIncarnation(0),graph.nodeIncarnation(1)};
  const auto local=glm::translate(glm::mat4(1),glm::vec3(2,3,4));
  auto prepared=graph.preparePlacement(1,0,local,0,graph.activeRoots());ASSERT_TRUE(prepared);
  EXPECT_EQ(prepared.candidateGraph()->nodeIncarnation(1),births[1]);
  AllocationGuard guard(true);
  const bool ready=graph.canPublishPlacement(prepared),published=graph.publishPlacement(prepared);
  const bool consumed=graph.canPublishPlacement(prepared);const auto attempts=guard.allocations();guard.disable();
  EXPECT_TRUE(ready);EXPECT_TRUE(published);EXPECT_FALSE(consumed);EXPECT_EQ(attempts,0u);
  EXPECT_EQ(graph.identity(),identity);EXPECT_EQ(graph.revision(),revision+1);
  EXPECT_EQ(graph.getNode(1)->localTransform,local);
  EXPECT_EQ(graph.nodeIncarnation(0),births[0]);EXPECT_EQ(graph.nodeIncarnation(1),births[1]);
}

TEST(SceneNodeLifetime, StaleAndForeignPlacementGuardAndDirectPublishRejectWithoutAllocation) {
  auto graph=graphFixture();auto copy=graph;
  auto prepared=graph.preparePlacement(1,0,glm::translate(glm::mat4(1),glm::vec3(1,2,3)),0,graph.activeRoots());ASSERT_TRUE(prepared);
  graph.setMaterialIndex(1,7);const auto before=graph.revision(),birth=graph.nodeIncarnation(1);
  AllocationGuard guard(true);
  const bool stale=graph.canPublishPlacement(prepared),foreign=copy.canPublishPlacement(prepared);
  const bool direct=graph.publishPlacement(prepared);const auto attempts=guard.allocations();guard.disable();
  EXPECT_FALSE(stale);EXPECT_FALSE(foreign);EXPECT_FALSE(direct);EXPECT_EQ(attempts,0u);
  EXPECT_EQ(graph.revision(),before);EXPECT_EQ(graph.nodeIncarnation(1),birth);
  EXPECT_EQ(graph.getNode(1)->materialIndex,7u);
}

TEST(SceneNodeLifetime, OriginalTailRestorationResumesBirthAndCutBranchCannotImpersonateIt) {
  auto graph=graphFixture();const auto original=graph.nodeIncarnation(1),parent=graph.nodeIncarnation(0);
  auto removed=graph.detachTail(1);ASSERT_TRUE(removed);EXPECT_EQ(graph.nodeIncarnation(1),0u);
  const auto replacement=graph.createNode(glm::mat4(1),0,false,SceneGraph::kInvalidNode,"Camera");
  graph.setParent(replacement,0);graph.updateWorldTransforms();
  EXPECT_EQ(replacement,1u);EXPECT_NE(graph.nodeIncarnation(1),original);
  auto cut=graph.detachTail(1);ASSERT_TRUE(cut);
  auto restore=graph.prepareRestoreTail(*removed);ASSERT_TRUE(restore);
  AllocationGuard guard(true);const bool restored=graph.publishTailChange(restore);
  const auto attempts=guard.allocations();guard.disable();
  EXPECT_TRUE(restored);EXPECT_EQ(attempts,0u);
  EXPECT_EQ(graph.nodeIncarnation(1),original);EXPECT_EQ(graph.nodeIncarnation(0),parent);
}

TEST(SceneNodeLifetime, TailCannotResumeUnderEqualButRebornRetainedParent) {
  auto graph=graphFixture();auto child=graph.detachTail(1);ASSERT_TRUE(child);
  const auto oldParent=*graph.getNode(0);const auto birth=graph.nodeIncarnation(0);
  auto parent=graph.detachTail(0);ASSERT_TRUE(parent);
  graph.createNode(glm::mat4(1),0,false,SceneGraph::kInvalidNode,"Parent");
  EXPECT_EQ(*graph.getNode(0),oldParent);EXPECT_NE(graph.nodeIncarnation(0),birth);
  auto refused=graph.prepareRestoreTail(*child);EXPECT_FALSE(refused);
  EXPECT_EQ(graph.nodeCount(),1u);
}

TEST(SceneNodeLifetime, AppendRequiresOriginalPrefixLifetimeNotMerelyEqualNodes) {
  SceneGraph graph;graph.createNode(glm::mat4(1),0,false,SceneGraph::kInvalidNode,"Retained");
  SceneGraph foreign;foreign.createNode(glm::mat4(1),0,false,SceneGraph::kInvalidNode,"Retained");
  foreign.createNode(glm::mat4(1),0,false,SceneGraph::kInvalidNode,"New");
  const auto birth=graph.nodeIncarnation(0);ASSERT_EQ(*graph.getNode(0),*foreign.getNode(0));
  EXPECT_THROW((void)graph.prepareAppend(std::move(foreign)),std::invalid_argument);
  auto candidate=graph.makeEmptyNodeCandidate(0,"New");const auto added=candidate.nodeIncarnation(1);
  auto proof=graph.prepareAppend(std::move(candidate));ASSERT_TRUE(graph.publishAppend(proof));
  EXPECT_EQ(graph.nodeIncarnation(0),birth);EXPECT_NE(added,birth);EXPECT_EQ(graph.nodeIncarnation(1),added);
}

TEST(SceneNodeLifetime, RetirementAndOriginalUndoKeepTheExactNodeLifetime) {
  auto graph=graphFixture();const auto birth=graph.nodeIncarnation(1);
  auto retire=graph.prepareRetireSubtree(1);ASSERT_TRUE(retire);
  ASSERT_TRUE(graph.publishRetirement(retire));auto receipt=retire.takeRetirement();ASSERT_TRUE(receipt);
  EXPECT_EQ(graph.nodeIncarnation(1),birth);EXPECT_TRUE(graph.isNodeRetired(1));
  auto resume=graph.prepareRetirementChange(*receipt,false);ASSERT_TRUE(resume);
  ASSERT_TRUE(graph.publishRetirement(resume));EXPECT_FALSE(graph.isNodeRetired(1));
  EXPECT_EQ(graph.nodeIncarnation(1),birth);
}

TEST(SceneNodeLifetime, EveryPlacementPreparationAllocationCutpointPreservesLiveBirthsAndPayload) {
  auto graph=graphFixture();const auto identity=graph.identity(),revision=graph.revision();
  const auto before0=*graph.getNode(0),before1=*graph.getNode(1);
  const std::array births{graph.nodeIncarnation(0),graph.nodeIncarnation(1)};
  const auto local=glm::translate(glm::mat4(1),glm::vec3(7,8,9));
  size_t total{};{
    AllocationGuard counter(false);auto proof=graph.preparePlacement(1,0,local,0,graph.activeRoots());
    total=counter.allocations();counter.disable();ASSERT_TRUE(proof);
  }
  ASSERT_GT(total,0u);ASSERT_LT(total,1024u);
  for(size_t allowed=0;allowed<total;++allowed) {
    bool failed{};{
      AllocationGuard guard(false,allowed);
      try {auto proof=graph.preparePlacement(1,0,local,0,graph.activeRoots());}
      catch(const std::bad_alloc&) {failed=true;}
      guard.disable();
    }
    EXPECT_TRUE(failed)<<allowed;EXPECT_EQ(graph.identity(),identity);EXPECT_EQ(graph.revision(),revision);
    EXPECT_EQ(*graph.getNode(0),before0);EXPECT_EQ(*graph.getNode(1),before1);
    EXPECT_EQ(graph.nodeIncarnation(0),births[0]);EXPECT_EQ(graph.nodeIncarnation(1),births[1]);
  }
}

TEST(SceneNodeLifetime, OriginalRetirementReceiptCannotDeleteAnEqualFreshBranch) {
  auto graph=graphFixture();const auto oldBirth=graph.nodeIncarnation(1);
  auto retire=graph.prepareRetireSubtree(1);ASSERT_TRUE(graph.publishRetirement(retire));
  auto receipt=retire.takeRetirement();ASSERT_TRUE(receipt);
  auto undo=graph.prepareRetirementChange(*receipt,false);ASSERT_TRUE(graph.publishRetirement(undo));
  const auto oldNode=*graph.getNode(1);auto cut=graph.detachTail(1);ASSERT_TRUE(cut);
  const auto replacement=graph.createNode(glm::mat4(1),0,false,SceneGraph::kInvalidNode,"Camera");
  graph.setParent(replacement,0);graph.updateWorldTransforms();
  ASSERT_EQ(*graph.getNode(1),oldNode);ASSERT_NE(graph.nodeIncarnation(1),oldBirth);
  auto refused=graph.prepareRetirementChange(*receipt,true);EXPECT_FALSE(refused);
  EXPECT_FALSE(graph.isNodeRetired(1));EXPECT_EQ(graph.nodeCount(),2u);
}
} // namespace
