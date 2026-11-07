#include <Container/utility/MaterialBindingAuthoring.h>

#include "../renderer/scene/material_publication_allocations.h"

#include <glm/gtc/matrix_transform.hpp>

#include <array>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <vector>

namespace {
using namespace container::scene;

struct Fixture {
  SceneGraph graph;
  std::vector<uint32_t> mapping{4,1};
  uint64_t asset{91};
  uint32_t owner{},first{},second{},procedural{};
  Fixture() {
    owner=graph.createNode(glm::translate(glm::mat4(1),glm::vec3(3,1,-2)),4,false,SceneGraph::kInvalidNode,"Imported group");
    graph.getNode(owner)->sourceNodeIndex=0;
    first=graph.createNode(glm::translate(glm::mat4(1),glm::vec3(1,0,0)),4,true,2,"First instance");
    second=graph.createNode(glm::translate(glm::mat4(1),glm::vec3(-1,0,0)),4,true,2,"Second instance");
    for(const auto id:{first,second}) {
      graph.setParent(id,owner);
      auto& node=*graph.getNode(id);node.sourceMeshIndex=0;node.sourcePrimitiveIndex=1;node.meshOwnerGraphNode=owner;
    }
    procedural=graph.createNode(glm::mat4(1),6,true,0,"Procedural cube");
    graph.updateWorldTransforms();
  }
  MaterialBindingContext context() const {return {asset,mapping,8,3};}
  MaterialBindingCommand plan(uint32_t node,uint32_t after) const {
    const std::array requests{MaterialBindingRequest{node,graph.getNode(node)->materialIndex,after}};
    return PlanMaterialBindings(graph,context(),requests);
  }
};

std::vector<SceneNode> nodes(const SceneGraph& graph) {
  std::vector<SceneNode> result;
  for(uint32_t i=0;i<graph.nodeCount();++i) result.push_back(*graph.getNode(i));
  return result;
}

TEST(MaterialBindingAuthoring, IsolatesOneSharedPrimitiveInstanceAndPreservesAllOtherPayloads) {
  Fixture f;const auto before=nodes(f.graph);const auto roots=f.graph.rootNodes();
  const auto renderables=f.graph.renderableNodes();const auto hierarchy=f.graph.hierarchyRevision();
  const auto revision=f.graph.revision();
  auto command=f.plan(f.first,1);ASSERT_TRUE(command);ASSERT_TRUE(command.changes());
  ASSERT_EQ(command.assignments().size(),1u);
  EXPECT_EQ(command.assignments()[0].beforeSourceMaterial,0);
  EXPECT_EQ(command.assignments()[0].afterSourceMaterial,1);
  auto prepared=PrepareMaterialBindings(f.graph,f.context(),command);
  EXPECT_EQ(nodes(f.graph),before);
  ASSERT_TRUE(PublishMaterialBindings(f.graph,prepared,f.context()));
  auto expected=before;expected[f.first].materialIndex=1;
  EXPECT_EQ(nodes(f.graph),expected);EXPECT_EQ(f.graph.revision(),revision+1);
  EXPECT_EQ(f.graph.hierarchyRevision(),hierarchy);EXPECT_EQ(f.graph.rootNodes(),roots);
  EXPECT_EQ(f.graph.renderableNodes(),renderables);EXPECT_FALSE(f.graph.activeRoots());
  EXPECT_EQ(f.graph.getNode(f.second)->materialIndex,4u);
}

TEST(MaterialBindingAuthoring, NoncontiguousSourceMappingAndActualProceduralDefaultAreRetained) {
  Fixture f;auto command=f.plan(f.procedural,4);
  ASSERT_EQ(command.assignments().size(),1u);
  EXPECT_EQ(command.assignments()[0].beforeRuntimeMaterial,6u);
  EXPECT_EQ(command.assignments()[0].beforeSourceMaterial,-1);
  EXPECT_EQ(command.assignments()[0].afterSourceMaterial,0);
  auto initial=PrepareMaterialBindings(f.graph,f.context(),command);
  ASSERT_TRUE(PublishMaterialBindings(f.graph,initial,f.context()));
  auto undo=RebaseMaterialBindings(f.graph,f.context(),command,MaterialBindingSide::Before);
  ASSERT_TRUE(PublishMaterialBindings(f.graph,undo,f.context()));
  EXPECT_EQ(f.graph.getNode(f.procedural)->materialIndex,6u);
}

TEST(MaterialBindingAuthoring, OneRevisionForSortedBatchIncludingUnchangedRows) {
  Fixture f;const auto revision=f.graph.revision();
  const std::array requests{MaterialBindingRequest{f.procedural,6,6},
      MaterialBindingRequest{f.second,4,1},MaterialBindingRequest{f.first,4,7}};
  const auto command=PlanMaterialBindings(f.graph,f.context(),requests);
  ASSERT_EQ(command.assignments().size(),3u);
  EXPECT_EQ(command.assignments()[0].graphNode,f.first);
  EXPECT_EQ(command.assignments()[2].graphNode,f.procedural);
  auto token=PrepareMaterialBindings(f.graph,f.context(),command);
  ASSERT_TRUE(PublishMaterialBindings(f.graph,token,f.context()));
  EXPECT_EQ(f.graph.revision(),revision+1);EXPECT_EQ(f.graph.getNode(f.first)->materialIndex,7u);
  EXPECT_EQ(f.graph.getNode(f.second)->materialIndex,1u);EXPECT_EQ(f.graph.getNode(f.procedural)->materialIndex,6u);
}

TEST(MaterialBindingAuthoring, NoopAndEmptyBatchesConsumeWithoutChangingAnyRevision) {
  Fixture f;
  for(const bool empty:{false,true}) {
    const std::array requests{MaterialBindingRequest{f.first,4,4}};
    const auto command=PlanMaterialBindings(f.graph,f.context(),empty?std::span<const MaterialBindingRequest>{}:std::span(requests));
    ASSERT_TRUE(command);EXPECT_FALSE(command.changes());const auto revision=f.graph.revision();
    auto prepared=PrepareMaterialBindings(f.graph,f.context(),command);ASSERT_TRUE(prepared);
    EXPECT_FALSE(prepared.changes());ASSERT_TRUE(PublishMaterialBindings(f.graph,prepared,f.context()));
    EXPECT_FALSE(prepared);EXPECT_FALSE(PublishMaterialBindings(f.graph,prepared,f.context()));
    EXPECT_EQ(f.graph.revision(),revision);
  }
}

TEST(MaterialBindingAuthoring, ReplayRebasesAcrossRenamePoseAndReparentWithoutRestoringTheirOldValues) {
  Fixture f;const auto command=f.plan(f.first,1);
  auto initial=PrepareMaterialBindings(f.graph,f.context(),command);
  ASSERT_TRUE(PublishMaterialBindings(f.graph,initial,f.context()));
  f.graph.setName(f.first,"Renamed after assignment");
  const auto edited=glm::translate(glm::mat4(1),glm::vec3(11,4,7));f.graph.setLocalTransform(f.first,edited);
  ASSERT_TRUE(f.graph.setParent(f.first,f.procedural));f.graph.updateWorldTransforms();
  const auto world=f.graph.getNode(f.first)->worldTransform;
  auto undo=RebaseMaterialBindings(f.graph,f.context(),command,MaterialBindingSide::Before);
  ASSERT_TRUE(PublishMaterialBindings(f.graph,undo,f.context()));
  EXPECT_EQ(f.graph.getNode(f.first)->name,"Renamed after assignment");
  EXPECT_EQ(f.graph.getNode(f.first)->localTransform,edited);EXPECT_EQ(f.graph.getNode(f.first)->worldTransform,world);
  EXPECT_EQ(f.graph.getNode(f.first)->parent,f.procedural);EXPECT_EQ(f.graph.getNode(f.first)->materialIndex,4u);
  auto redo=RebaseMaterialBindings(f.graph,f.context(),command,MaterialBindingSide::After);
  ASSERT_TRUE(PublishMaterialBindings(f.graph,redo,f.context()));EXPECT_EQ(f.graph.getNode(f.first)->materialIndex,1u);
}

TEST(MaterialBindingAuthoring, WrongHistorySideAndInterveningMaterialEditRejectWithoutMutation) {
  Fixture f;const auto command=f.plan(f.first,1);const auto before=nodes(f.graph);
  EXPECT_THROW(RebaseMaterialBindings(f.graph,f.context(),command,MaterialBindingSide::Before),std::invalid_argument);
  f.graph.setMaterialIndex(f.first,7);const auto edited=nodes(f.graph);
  EXPECT_THROW(PrepareMaterialBindings(f.graph,f.context(),command),std::invalid_argument);
  EXPECT_EQ(nodes(f.graph),edited);EXPECT_NE(edited,before);
}

TEST(MaterialBindingAuthoring, ProspectiveAppendUsesIndependentTailIdsAndExactBeforeAfterContexts) {
  Fixture f;const std::array requests{MaterialBindingRequest{f.first,4,8}};
  const auto command=PlanMaterialBindings(f.graph,f.context(),requests,MaterialBindingAppendTarget{2,8});
  ASSERT_EQ(command.context(MaterialBindingSide::After).sourceMaterialToRuntime.size(),3u);
  EXPECT_EQ(command.context(MaterialBindingSide::After).runtimeMaterialCount,9u);
  EXPECT_EQ(command.assignments()[0].afterSourceMaterial,2);
  auto initial=PrepareMaterialBindings(f.graph,f.context(),command);
  ASSERT_TRUE(PublishMaterialBindings(f.graph,initial,f.context()));
  // Simulate the outer no-fail phase's separately proved material publication.
  f.mapping.push_back(8);auto after=f.context();after.runtimeMaterialCount=9;
  EXPECT_THROW(PrepareMaterialBindings(f.graph,after,command),std::invalid_argument);
  auto undo=RebaseMaterialBindings(f.graph,after,command,MaterialBindingSide::Before);
  ASSERT_TRUE(PublishMaterialBindings(f.graph,undo,after));f.mapping.pop_back();
  EXPECT_EQ(f.graph.getNode(f.first)->materialIndex,4u);
  auto redo=RebaseMaterialBindings(f.graph,f.context(),command,MaterialBindingSide::After);
  ASSERT_TRUE(PublishMaterialBindings(f.graph,redo,f.context()));EXPECT_EQ(f.graph.getNode(f.first)->materialIndex,8u);
}

TEST(MaterialBindingAuthoring, InvalidAppendAndUnavailableReplacementRejectBeforeAnyPublication) {
  Fixture f;const std::array requests{MaterialBindingRequest{f.first,4,8}};const auto before=nodes(f.graph);
  EXPECT_THROW(PlanMaterialBindings(f.graph,f.context(),requests),std::invalid_argument);
  for(const auto append:{MaterialBindingAppendTarget{1,8},MaterialBindingAppendTarget{2,7},MaterialBindingAppendTarget{3,9}})
    EXPECT_THROW(PlanMaterialBindings(f.graph,f.context(),requests,append),std::invalid_argument);
  EXPECT_EQ(nodes(f.graph),before);EXPECT_EQ(f.mapping,(std::vector<uint32_t>{4,1}));
}

TEST(MaterialBindingAuthoring, GenericBatchMayEditInactiveAndHiddenBindingsWithoutActivatingThem) {
  Fixture f;const std::array roots{f.procedural};f.graph.setActiveRoots(roots);f.graph.setVisible(f.first,false);
  ASSERT_FALSE(f.graph.isNodeActive(f.first));const auto active=f.graph.activeRoots();
  const auto visible=f.graph.renderableNodes();const auto command=f.plan(f.first,1);
  auto token=PrepareMaterialBindings(f.graph,f.context(),command);
  ASSERT_TRUE(PublishMaterialBindings(f.graph,token,f.context()));
  EXPECT_EQ(f.graph.activeRoots(),active);EXPECT_EQ(f.graph.renderableNodes(),visible);
  EXPECT_FALSE(f.graph.isNodeActive(f.first));EXPECT_FALSE(f.graph.getNode(f.first)->visible);
}

TEST(MaterialBindingAuthoring, DuplicatedRequestsRejectEvenIfTheyAreBothNoops) {
  Fixture f;const std::array requests{MaterialBindingRequest{f.first,4,4},MaterialBindingRequest{f.first,4,4}};
  const auto before=nodes(f.graph);EXPECT_THROW(PlanMaterialBindings(f.graph,f.context(),requests),std::invalid_argument);
  EXPECT_EQ(nodes(f.graph),before);
}

TEST(MaterialBindingAuthoring, MissingNonrenderableRetiredAndInvalidPrimitiveBindingsReject) {
  Fixture f;
  for(const auto id:{f.owner,SceneGraph::kInvalidNode}) {
    const std::array request{MaterialBindingRequest{id,4,1}};
    EXPECT_THROW(PlanMaterialBindings(f.graph,f.context(),request),std::invalid_argument);
  }
  f.graph.getNode(f.first)->retired=true;
  const std::array request{MaterialBindingRequest{f.first,4,1}};
  EXPECT_THROW(PlanMaterialBindings(f.graph,f.context(),request),std::invalid_argument);
  f.graph.getNode(f.first)->retired=false;f.graph.getNode(f.first)->primitiveIndex=3;
  EXPECT_THROW(PlanMaterialBindings(f.graph,f.context(),request),std::invalid_argument);
}

TEST(MaterialBindingAuthoring, InvalidSourceMapsAndMaterialBoundsReject) {
  Fixture f;const std::array request{MaterialBindingRequest{f.first,4,1}};
  for(const auto map:{std::vector<uint32_t>{4,4},std::vector<uint32_t>{4,8}}) {
    auto context=f.context();context.sourceMaterialToRuntime=map;
    EXPECT_THROW(PlanMaterialBindings(f.graph,context,request),std::invalid_argument);
  }
  auto context=f.context();context.assetIdentity=0;
  EXPECT_THROW(PlanMaterialBindings(f.graph,context,request),std::invalid_argument);
  context=f.context();context.runtimeMaterialCount=UINT32_MAX;
  EXPECT_THROW(PlanMaterialBindings(f.graph,context,request),std::invalid_argument);
  const std::array stale{MaterialBindingRequest{f.first,0,1}};
  EXPECT_THROW(PlanMaterialBindings(f.graph,f.context(),stale),std::invalid_argument);
}

TEST(MaterialBindingAuthoring, FreshTokenRejectsImmediateRevisionChangeWithoutInvalidatingHistory) {
  Fixture f;const auto command=f.plan(f.first,1);auto stale=PrepareMaterialBindings(f.graph,f.context(),command);
  f.graph.setName(f.second,"An unrelated rename");const auto before=nodes(f.graph);
  EXPECT_FALSE(CanPublishMaterialBindings(f.graph,stale,f.context()));
  EXPECT_FALSE(PublishMaterialBindings(f.graph,stale,f.context()));EXPECT_EQ(nodes(f.graph),before);
  auto fresh=PrepareMaterialBindings(f.graph,f.context(),command);ASSERT_TRUE(PublishMaterialBindings(f.graph,fresh,f.context()));
}

TEST(MaterialBindingAuthoring, ForeignCopiedAndReplacementGraphsCannotUseCommandOrToken) {
  Fixture f;const auto command=f.plan(f.first,1);auto token=PrepareMaterialBindings(f.graph,f.context(),command);
  SceneGraph copy=f.graph;
  EXPECT_THROW(PrepareMaterialBindings(copy,f.context(),command),std::invalid_argument);
  EXPECT_FALSE(PublishMaterialBindings(copy,token,f.context()));
  f.graph=SceneGraph{};
  EXPECT_FALSE(PublishMaterialBindings(f.graph,token,f.context()));
}

TEST(MaterialBindingAuthoring, AssetMappingAndCountChangesRejectTheCompleteBatchAtomically) {
  Fixture f;const std::array requests{MaterialBindingRequest{f.first,4,1},MaterialBindingRequest{f.second,4,7}};
  const auto command=PlanMaterialBindings(f.graph,f.context(),requests);auto token=PrepareMaterialBindings(f.graph,f.context(),command);
  const auto before=nodes(f.graph);
  auto altered=f.context();++altered.assetIdentity;
  EXPECT_FALSE(PublishMaterialBindings(f.graph,token,altered));
  altered=f.context();++altered.runtimeMaterialCount;
  EXPECT_FALSE(PublishMaterialBindings(f.graph,token,altered));
  altered=f.context();++altered.runtimePrimitiveCount;
  EXPECT_FALSE(PublishMaterialBindings(f.graph,token,altered));
  std::array<uint32_t,2> reordered{1,4};altered=f.context();altered.sourceMaterialToRuntime=reordered;
  EXPECT_FALSE(PublishMaterialBindings(f.graph,token,altered));EXPECT_EQ(nodes(f.graph),before);
  ASSERT_TRUE(PublishMaterialBindings(f.graph,token,f.context()));
}

TEST(MaterialBindingAuthoring, RawMutationOfLastRowIsCaughtBeforeFirstRowIsWritten) {
  Fixture f;const std::array requests{MaterialBindingRequest{f.first,4,1},MaterialBindingRequest{f.second,4,7}};
  const auto command=PlanMaterialBindings(f.graph,f.context(),requests);auto token=PrepareMaterialBindings(f.graph,f.context(),command);
  const auto revision=f.graph.revision();f.graph.getNode(f.second)->sourceInstanceIndex=11;
  EXPECT_FALSE(CanPublishMaterialBindings(f.graph,token,f.context()));
  EXPECT_FALSE(PublishMaterialBindings(f.graph,token,f.context()));
  EXPECT_EQ(f.graph.getNode(f.first)->materialIndex,4u);EXPECT_EQ(f.graph.getNode(f.second)->materialIndex,4u);
  EXPECT_EQ(f.graph.revision(),revision);
}

TEST(MaterialBindingAuthoring, RetirementAndOwnerLossRejectStagedAndReplayedBindings) {
  Fixture f;const auto command=f.plan(f.first,1);auto token=PrepareMaterialBindings(f.graph,f.context(),command);
  f.graph.getNode(f.owner)->retired=true;
  EXPECT_FALSE(PublishMaterialBindings(f.graph,token,f.context()));
  EXPECT_THROW(PrepareMaterialBindings(f.graph,f.context(),command),std::invalid_argument);
  f.graph.getNode(f.owner)->retired=false;f.graph.getNode(f.first)->meshOwnerGraphNode=SceneGraph::kInvalidNode;
  EXPECT_FALSE(PublishMaterialBindings(f.graph,token,f.context()));
  EXPECT_THROW(PrepareMaterialBindings(f.graph,f.context(),command),std::invalid_argument);
}

TEST(MaterialBindingAuthoring, OutputScratchAndCountLimitsRejectWithoutMutatingInputs) {
  Fixture f;const std::array requests{MaterialBindingRequest{f.first,4,1},MaterialBindingRequest{f.second,4,7}};
  const auto before=nodes(f.graph);
  for(const int kind:{0,1,2,3,4}) {
    MaterialBindingLimits limits;
    if(kind==0) limits.maxOutputBytes=1;
    if(kind==1) limits.maxScratchBytes=1;
    if(kind==2) limits.maxBindings=1;
    if(kind==3) limits.maxSourceMaterials=1;
    if(kind==4) limits.maxRuntimeMaterials=7;
    EXPECT_THROW(PlanMaterialBindings(f.graph,f.context(),requests,{},limits),std::invalid_argument);
  }
  const auto command=PlanMaterialBindings(f.graph,f.context(),requests);MaterialBindingLimits tiny;tiny.maxOutputBytes=command.ownedBytes();
  EXPECT_THROW(PrepareMaterialBindings(f.graph,f.context(),command,tiny),std::invalid_argument);
  EXPECT_EQ(nodes(f.graph),before);
}

TEST(MaterialBindingAuthoring, CommandCopyMoveAndOneShotTokenKeepTheirOwnedIdentityStorage) {
  Fixture f;auto original=f.plan(f.first,1);auto command=original;original={};
  auto token=PrepareMaterialBindings(f.graph,f.context(),command);auto moved=std::move(token);
  EXPECT_FALSE(token);ASSERT_TRUE(moved);ASSERT_TRUE(PublishMaterialBindings(f.graph,moved,f.context()));
  EXPECT_FALSE(moved);EXPECT_FALSE(CanPublishMaterialBindings(f.graph,moved,f.context()));
  EXPECT_FALSE(PublishMaterialBindings(f.graph,moved,f.context()));
  auto undo=RebaseMaterialBindings(f.graph,f.context(),command,MaterialBindingSide::Before);
  ASSERT_TRUE(PublishMaterialBindings(f.graph,undo,f.context()));
}

TEST(MaterialBindingAuthoring, RetainedRoutingReceiptOutlivesMovedConsumedAndDestroyedGraphToken) {
  Fixture f;MaterialBindingCommand receipt;MaterialBindingSide target{};
  uint64_t expected{},candidate{};
  {
    auto command=f.plan(f.first,1);auto token=PrepareMaterialBindings(f.graph,f.context(),command);
    receipt=token.command();target=token.targetSide();expected=token.expectedGraphRevision();candidate=token.candidateGraphRevision();
    EXPECT_EQ(token.graphIdentity(),f.graph.identity());EXPECT_EQ(candidate,expected+1);
    auto moved=std::move(token);ASSERT_TRUE(PublishMaterialBindings(f.graph,moved,f.context()));
    EXPECT_TRUE(MatchesMaterialBindings(f.graph,receipt,f.context(),target));
  }
  EXPECT_EQ(f.graph.revision(),candidate);
  EXPECT_FALSE(MatchesMaterialBindings(f.graph,receipt,f.context(),MaterialBindingSide::Before));
  f.graph.setName(f.first,"Name is outside the material receipt");
  EXPECT_TRUE(MatchesMaterialBindings(f.graph,receipt,f.context(),target));
  f.graph.getNode(f.first)->materialIndex=7;
  EXPECT_FALSE(MatchesMaterialBindings(f.graph,receipt,f.context(),target));
}

TEST(MaterialBindingAuthoring, ReceiptOwnsItsRequestsAndContextInsteadOfBorrowingMutableInput) {
  Fixture f;std::array request{MaterialBindingRequest{f.first,4,1}};
  const auto command=PlanMaterialBindings(f.graph,f.context(),request);
  request[0]={f.second,4,7};f.mapping[0]=7;
  EXPECT_EQ(command.assignments()[0].graphNode,f.first);EXPECT_EQ(command.assignments()[0].afterRuntimeMaterial,1u);
  EXPECT_EQ(command.context(MaterialBindingSide::Before).sourceMaterialToRuntime[0],4u);
  EXPECT_FALSE(MatchesMaterialBindings(f.graph,command,f.context(),MaterialBindingSide::Before));
  f.mapping[0]=4;EXPECT_TRUE(MatchesMaterialBindings(f.graph,command,f.context(),MaterialBindingSide::Before));
}

TEST(MaterialBindingAuthoring, PublicationAndStalePredicatesPerformNoAllocation) {
  Fixture f;const std::array requests{MaterialBindingRequest{f.first,4,1},MaterialBindingRequest{f.second,4,7}};
  const auto command=PlanMaterialBindings(f.graph,f.context(),requests);auto token=PrepareMaterialBindings(f.graph,f.context(),command);
  const auto context=f.context();bool before{},published{},consumed{},matched{};
  MaterialBindingCommand retained;
  material_publication_test::AllocationGuard guard(true);
  retained=token.command();
  before=CanPublishMaterialBindings(f.graph,token,context);
  published=PublishMaterialBindings(f.graph,token,context);
  consumed=!CanPublishMaterialBindings(f.graph,token,context) && !PublishMaterialBindings(f.graph,token,context);
  matched=MatchesMaterialBindings(f.graph,retained,context,token.targetSide());
  guard.disable();
  EXPECT_TRUE(before);EXPECT_TRUE(published);EXPECT_TRUE(consumed);EXPECT_TRUE(matched);EXPECT_EQ(guard.allocations(),0u);
  EXPECT_EQ(f.graph.getNode(f.first)->materialIndex,1u);EXPECT_EQ(f.graph.getNode(f.second)->materialIndex,7u);
}

TEST(MaterialBindingAuthoring, ReceiptIsImmutableAndPublicationIsNoexcept) {
  static_assert(std::is_copy_constructible_v<MaterialBindingCommand>);
  static_assert(!std::is_copy_constructible_v<PreparedMaterialBindingChange>);
  static_assert(std::is_same_v<decltype(std::declval<const MaterialBindingCommand&>().assignments()),
      std::span<const MaterialBindingAssignment>>);
  static_assert(noexcept(CanPublishMaterialBindings(std::declval<const SceneGraph&>(),
      std::declval<const PreparedMaterialBindingChange&>(),std::declval<MaterialBindingContext>())));
  static_assert(noexcept(PublishMaterialBindings(std::declval<SceneGraph&>(),
      std::declval<PreparedMaterialBindingChange&>(),std::declval<MaterialBindingContext>())));
  MaterialBindingCommand command;PreparedMaterialBindingChange prepared;SceneGraph graph;
  EXPECT_FALSE(command);EXPECT_FALSE(prepared);EXPECT_FALSE(command.changes());EXPECT_FALSE(prepared.changes());
  EXPECT_TRUE(command.assignments().empty());EXPECT_TRUE(prepared.assignments().empty());
  EXPECT_THROW(PrepareMaterialBindings(graph,{1,{},0,0},command),std::invalid_argument);
  EXPECT_FALSE(PublishMaterialBindings(graph,prepared,{1,{},0,0}));
}
} // namespace
