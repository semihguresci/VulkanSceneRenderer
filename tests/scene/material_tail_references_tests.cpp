#include <Container/utility/MaterialTailReferences.h>

#include "../renderer/scene/material_publication_allocations.h"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace container::scene;
using namespace container::geometry::gltf;
using V=tinygltf::Value;
using O=V::Object;
using A=V::Array;

struct Fixture {
  tinygltf::Model authored,evaluated;
  SceneGraph graph;
  MaterialVariantLibrary variants;
  std::vector<uint32_t> mapping{2,6},protectedIds{0,1};
  uint64_t asset{79};uint32_t group{},leaf{};
  Fixture() {
    authored.asset.version="2.0";authored.materials.resize(2);
    authored.materials[0].name="Retained";authored.materials[1].name="Owned tail";
    tinygltf::Primitive primitive;primitive.material=0;tinygltf::Mesh mesh;mesh.primitives.push_back(primitive);
    authored.meshes.push_back(mesh);evaluated=authored;variants=ParseMaterialVariants(authored);
    // Nonrenderable material placeholders are deliberately NOT real bindings.
    group=graph.createNode(glm::mat4(1),6,false,SceneGraph::kInvalidNode,"Group");
    leaf=graph.createNode(glm::mat4(1),2,true,0,"Renderable");graph.setParent(leaf,group);graph.updateWorldTransforms();
  }
  MaterialBindingContext context() const {return {asset,mapping,7,1};}
  MaterialTailReferenceView view() const {return {authored,evaluated,graph,variants,context(),1,6,protectedIds};}
  void sync() {evaluated=authored;variants=ParseMaterialVariants(authored);}
  void pointer(std::string path) {
    authored.extensionsUsed.push_back("KHR_animation_pointer");tinygltf::AnimationChannel channel;
    channel.target_node=-1;channel.target_path="pointer";
    channel.target_extensions["KHR_animation_pointer"]=V(O{{"pointer",V(std::move(path))}});
    tinygltf::Animation animation;animation.channels.push_back(channel);authored.animations.push_back(animation);evaluated=authored;
  }
  void variant(int material) {
    if(std::ranges::find(authored.extensionsUsed,"KHR_materials_variants")==authored.extensionsUsed.end())
      authored.extensionsUsed.push_back("KHR_materials_variants");
    authored.extensions["KHR_materials_variants"]=V(O{{"variants",V(A{V(O{{"name",V(std::string("Option"))}})})}});
    authored.meshes[0].primitives[0].extensions["KHR_materials_variants"]=V(O{{"mappings",V(A{
        V(O{{"material",V(material)},{"variants",V(A{V(0)})}})})}});
    sync();
  }
};

TEST(MaterialTailReferences, UnusedTailPassesWithoutCopyingOrMutatingModelsOrGraph) {
  Fixture f;const auto revision=f.graph.revision();const auto source=f.authored;
  const auto audit=AuditMaterialTailRemoval(f.view());ASSERT_TRUE(audit);
  EXPECT_EQ(audit.expectedGraphRevision(),revision);EXPECT_EQ(audit.candidateGraphRevision(),revision);
  EXPECT_EQ(audit.stats().graphRows,f.graph.nodeCount());EXPECT_GT(audit.stats().primitives,0u);
  EXPECT_TRUE(CanRemoveMaterialTail(f.view(),audit,MaterialTailAuditPhase::BeforeBindings));
  EXPECT_TRUE(CanRemoveMaterialTail(f.view(),audit,MaterialTailAuditPhase::AfterBindings));
  EXPECT_EQ(f.authored,source);EXPECT_EQ(f.graph.revision(),revision);
}

TEST(MaterialTailReferences, RenderableReferenceBlocksEvenWhenHiddenInactiveOrRetired) {
  for(const int mode:{0,1,2,3}) {
    Fixture f;f.graph.setMaterialIndex(f.leaf,6);
    if(mode==1) f.graph.setVisible(f.leaf,false);
    if(mode==2) {const std::array<uint32_t,0> empty{};f.graph.setActiveRoots(empty);}
    if(mode==3) f.graph.getNode(f.leaf)->retired=true;
    EXPECT_THROW(AuditMaterialTailRemoval(f.view()),std::invalid_argument);
  }
}

TEST(MaterialTailReferences, ProspectiveUndoOwnsBindingProofAndChecksBothPublicationPhases) {
  Fixture f;const std::array<uint32_t,1> beforeMap{2};
  const MaterialBindingContext before{f.asset,beforeMap,6,1};
  const auto command=PlanMaterialBindings(f.graph,before,std::array{MaterialBindingRequest{f.leaf,2,6}},MaterialBindingAppendTarget{1,6});
  auto create=PrepareMaterialBindings(f.graph,before,command);ASSERT_TRUE(PublishMaterialBindings(f.graph,create,before));
  auto undo=RebaseMaterialBindings(f.graph,f.context(),command,MaterialBindingSide::Before);
  const auto audit=AuditMaterialTailRemoval(f.view(),&undo);
  EXPECT_EQ(audit.candidateGraphRevision(),f.graph.revision()+1);
  ASSERT_TRUE(CanRemoveMaterialTail(f.view(),audit,MaterialTailAuditPhase::BeforeBindings));
  EXPECT_FALSE(CanRemoveMaterialTail(f.view(),audit,MaterialTailAuditPhase::AfterBindings));
  ASSERT_TRUE(PublishMaterialBindings(f.graph,undo,f.context()));undo={};
  EXPECT_FALSE(CanRemoveMaterialTail(f.view(),audit,MaterialTailAuditPhase::BeforeBindings));
  EXPECT_TRUE(CanRemoveMaterialTail(f.view(),audit,MaterialTailAuditPhase::AfterBindings));
  // Manager removal must happen AFTER that final audit, never before it.
  f.authored.materials.pop_back();f.evaluated.materials.pop_back();f.mapping.pop_back();
  EXPECT_FALSE(CanRemoveMaterialTail(f.view(),audit,MaterialTailAuditPhase::AfterBindings));
}

TEST(MaterialTailReferences, ProjectionDoesNotIgnoreOtherPublishedTailBindings) {
  Fixture f;f.graph.setMaterialIndex(f.leaf,6);
  const auto other=f.graph.createNode(glm::mat4(1),6,true,0,"Other hidden owner");f.graph.setVisible(other,false);
  const auto command=PlanMaterialBindings(f.graph,f.context(),std::array{MaterialBindingRequest{f.leaf,6,2}});
  auto change=PrepareMaterialBindings(f.graph,f.context(),command);
  EXPECT_THROW(AuditMaterialTailRemoval(f.view(),&change),std::invalid_argument);
  EXPECT_EQ(f.graph.getNode(f.leaf)->materialIndex,6u);
}

TEST(MaterialTailReferences, SourcePrimitiveReferencesInEitherSnapshotBlockRemoval) {
  for(const bool authored:{false,true}) {
    Fixture f;(authored?f.authored:f.evaluated).meshes[0].primitives[0].material=1;
    EXPECT_THROW(AuditMaterialTailRemoval(f.view()),std::invalid_argument);
  }
  Fixture f;f.authored.meshes[0].primitives[0].material=-1;f.sync();
  EXPECT_TRUE(AuditMaterialTailRemoval(f.view()));
}

TEST(MaterialTailReferences, RawAndDerivedVariantReferencesAreBothAudited) {
  Fixture f;f.variant(1);EXPECT_THROW(AuditMaterialTailRemoval(f.view()),std::invalid_argument);
  Fixture g;g.variant(0);const auto audit=AuditMaterialTailRemoval(g.view());EXPECT_TRUE(audit);
  g.authored.meshes[0].primitives[0].extensions["KHR_materials_variants"].Get<O>()["mappings"].Get<A>()[0].Get<O>()["material"]=V(1);
  EXPECT_FALSE(CanRemoveMaterialTail(g.view(),audit,MaterialTailAuditPhase::BeforeBindings));
  // Restore raw models but retain a library whose mapping still names the tail.
  Fixture h;h.variant(1);auto library=h.variants;h.variant(0);
  const MaterialTailReferenceView view{h.authored,h.evaluated,h.graph,library,h.context(),1,6,h.protectedIds};
  EXPECT_THROW(AuditMaterialTailRemoval(view),std::invalid_argument);
}

TEST(MaterialTailReferences, ParsedVariantDefaultCannotHideBehindUnchangedSourcePrimitive) {
  Fixture f;f.authored.meshes[0].primitives[0].material=1;f.sync();const auto retained=f.variants;
  f.authored.meshes[0].primitives[0].material=0;f.evaluated=f.authored;
  const MaterialTailReferenceView view{f.authored,f.evaluated,f.graph,retained,f.context(),1,6,f.protectedIds};
  EXPECT_THROW(AuditMaterialTailRemoval(view),std::invalid_argument);
}

TEST(MaterialTailReferences, TypedAndRawMaterialLodReferencesBlockButRemovedOutgoingReferencesDoNot) {
  Fixture f;f.authored.materials[0].lods={1};f.evaluated=f.authored;
  EXPECT_THROW(AuditMaterialTailRemoval(f.view()),std::invalid_argument);
  f.authored.materials[0].lods.clear();f.authored.extensionsUsed.push_back("MSFT_lod");
  f.authored.materials[0].extensions["MSFT_lod"]=V(O{{"ids",V(A{V(1)})}});f.evaluated=f.authored;
  EXPECT_THROW(AuditMaterialTailRemoval(f.view()),std::invalid_argument);
  f.authored.materials[0].extensions.clear();f.authored.materials[1].lods={0,1};
  f.authored.materials[1].extensions["MSFT_lod"]=V(O{{"ids",V(A{V(1)})}});f.evaluated=f.authored;
  EXPECT_TRUE(AuditMaterialTailRemoval(f.view()));
}

TEST(MaterialTailReferences, RawAnimationPointerBlocksTailForEveryPropertySuffix) {
  for(const auto* suffix:{"/pbrMetallicRoughness/baseColorFactor","/extensions/VENDOR_future/property","","/extras/material"}) {
    Fixture f;f.pointer(std::string("/materials/1")+suffix);
    EXPECT_THROW(AuditMaterialTailRemoval(f.view()),std::invalid_argument);
  }
  Fixture f;f.pointer("/materials/0/extensions/VENDOR_future/property");
  EXPECT_TRUE(AuditMaterialTailRemoval(f.view()));
}

TEST(MaterialTailReferences, MalformedPointersAndUnprovedReferenceRootsReject) {
  for(const auto* path:{"/materials/01/value","/materials/-1/value","/materials/2/value","/materials/1x/value",
      "/materials/0/value~2","/meshes/0/primitives/0/material"}) {
    Fixture f;f.pointer(path);EXPECT_THROW(AuditMaterialTailRemoval(f.view()),std::invalid_argument);
  }
}

TEST(MaterialTailReferences, ProtectedRuntimeIdentitiesAndAliasedSourceTailReject) {
  Fixture f;f.protectedIds.push_back(6);EXPECT_THROW(AuditMaterialTailRemoval(f.view()),std::invalid_argument);
  f.protectedIds={0,1};f.mapping[0]=6;EXPECT_THROW(AuditMaterialTailRemoval(f.view()),std::invalid_argument);
  f.mapping[0]=2;auto view=f.view();view.runtimeTail=5;
  EXPECT_THROW(AuditMaterialTailRemoval(view),std::invalid_argument);
}

TEST(MaterialTailReferences, UnknownRetainedExtensionsRejectWhileExtrasAreAnnotations) {
  Fixture f;f.authored.extras=V(O{{"material",V(1)},{"extensions",V(O{{"VENDOR_reference",V(O{{"material",V(1)}})}})}});
  f.evaluated=f.authored;EXPECT_TRUE(AuditMaterialTailRemoval(f.view()));
  f.authored.extensionsUsed.push_back("VENDOR_reference");f.authored.materials[0].extensions["VENDOR_reference"]=V(O{{"material",V(1)}});
  f.evaluated=f.authored;EXPECT_THROW(AuditMaterialTailRemoval(f.view()),std::invalid_argument);
}

TEST(MaterialTailReferences, UnknownNestedExtensionOutsideExtrasIsNotSilentlyIgnored) {
  Fixture f;f.authored.extensionsUsed={"KHR_materials_clearcoat","VENDOR_reference"};
  f.authored.materials[0].extensions["KHR_materials_clearcoat"]=V(O{{"clearcoatTexture",V(O{
      {"extensions",V(O{{"VENDOR_reference",V(O{{"material",V(1)}})}})}})}});
  f.evaluated=f.authored;EXPECT_THROW(AuditMaterialTailRemoval(f.view()),std::invalid_argument);
}

TEST(MaterialTailReferences, UnknownExtensionDiagnosticDoesNotCopyAnUnboundedName) {
  Fixture f;const std::string name="VENDOR_"+std::string(32768,'x');
  f.authored.materials[0].extensions[name]=V(O{});f.evaluated=f.authored;
  try {
    (void)AuditMaterialTailRemoval(f.view());
    FAIL()<<"Unknown retained extension was accepted";
  } catch(const std::invalid_argument& error) {
    const std::string message=error.what();
    EXPECT_LT(message.size(),512u);
    EXPECT_NE(message.find(name.substr(0,256)+"..."),std::string::npos);
  }
}

TEST(MaterialTailReferences, SupportedNestedTextureTransformsRemainAcceptedWithoutPredicateAllocation) {
  Fixture f;f.authored.extensionsUsed={"KHR_materials_clearcoat","KHR_texture_transform"};
  f.authored.materials[0].extensions["KHR_materials_clearcoat"]=V(O{{"clearcoatNormalTexture",V(O{
      {"index",V(0)},{"texCoord",V(7)},
      {"extensions",V(O{{"KHR_texture_transform",V(O{{"texCoord",V(1)}})}})},
      {"extras",V(O{{"material",V(1)}})}})}});
  f.evaluated=f.authored;const auto audit=AuditMaterialTailRemoval(f.view());const auto view=f.view();
  bool accepted{};material_publication_test::AllocationGuard guard(true);
  accepted=CanRemoveMaterialTail(view,audit,MaterialTailAuditPhase::BeforeBindings);guard.disable();
  EXPECT_TRUE(accepted);EXPECT_EQ(guard.allocations(),0u);
}

TEST(MaterialTailReferences, RemovedMaterialUnknownMetadataHasNoRetainedOutgoingReference) {
  Fixture f;f.authored.materials[1].extensions["VENDOR_reference"]=V(O{{"material",V(1)}});
  f.authored.materials[1].normalTexture.extensions["VENDOR_texture_note"]=V(O{{"material",V(1)}});
  f.evaluated=f.authored;EXPECT_TRUE(AuditMaterialTailRemoval(f.view()));
}

TEST(MaterialTailReferences, FinalPredicateRescansRawSourceChangesWithoutRelyingOnGraphRevision) {
  Fixture f;const auto audit=AuditMaterialTailRemoval(f.view());const auto revision=f.graph.revision();
  f.evaluated.materials[0].lods.push_back(1);
  EXPECT_EQ(f.graph.revision(),revision);EXPECT_FALSE(CanRemoveMaterialTail(f.view(),audit,MaterialTailAuditPhase::BeforeBindings));
  f.evaluated.materials[0].lods.clear();EXPECT_TRUE(CanRemoveMaterialTail(f.view(),audit,MaterialTailAuditPhase::BeforeBindings));
  f.graph.getNode(f.leaf)->materialIndex=6;
  EXPECT_FALSE(CanRemoveMaterialTail(f.view(),audit,MaterialTailAuditPhase::BeforeBindings));
}

TEST(MaterialTailReferences, ForeignOwnersContextChangesAndGraphRevisionChangesInvalidateAudit) {
  Fixture f;const auto audit=AuditMaterialTailRemoval(f.view());
  auto copied=f.authored;const MaterialTailReferenceView foreign{copied,f.evaluated,f.graph,f.variants,f.context(),1,6,f.protectedIds};
  EXPECT_FALSE(CanRemoveMaterialTail(foreign,audit,MaterialTailAuditPhase::BeforeBindings));
  ++f.asset;EXPECT_FALSE(CanRemoveMaterialTail(f.view(),audit,MaterialTailAuditPhase::BeforeBindings));--f.asset;
  f.protectedIds.push_back(3);EXPECT_FALSE(CanRemoveMaterialTail(f.view(),audit,MaterialTailAuditPhase::BeforeBindings));f.protectedIds.pop_back();
  f.graph.setName(f.group,"Unrelated rename requires fresh immediate audit");
  EXPECT_FALSE(CanRemoveMaterialTail(f.view(),audit,MaterialTailAuditPhase::BeforeBindings));
}

TEST(MaterialTailReferences, CountsMetadataDepthWorkAndOwnedStorageAreBoundedBeforeProofAllocation) {
  Fixture f;
  for(const int kind:{0,1,2,3,4,5,6}) {
    MaterialTailReferenceLimits limits;
    if(kind==0) limits.maxOwnedBytes=1;
    if(kind==1) limits.maxGraphRows=1;
    if(kind==2) limits.maxMetadataBytes=1;
    if(kind==3) limits.maxTraversalSteps=1;
    if(kind==4) limits.maxPrimitives=1;
    if(kind==5) limits.maxObjects=1;
    if(kind==6) limits.maxReferences=1;
    EXPECT_THROW(AuditMaterialTailRemoval(f.view(),nullptr,limits),std::invalid_argument);
  }
  f.authored.extras=V(O{{"nested",V(O{{"nested",V(O{{"deep",V(0)}})}})}});f.evaluated=f.authored;
  MaterialTailReferenceLimits shallow;shallow.maxMetadataDepth=1;
  EXPECT_THROW(AuditMaterialTailRemoval(f.view(),nullptr,shallow),std::invalid_argument);
}

TEST(MaterialTailReferences, HiddenInactiveMetadataStorageCannotBypassTheBoundedWalker) {
  Fixture f;f.authored.extras=V(0.);f.authored.extras.Get<A>().push_back(V(std::string(4096,'x')));f.evaluated=f.authored;
  EXPECT_THROW(AuditMaterialTailRemoval(f.view()),std::invalid_argument);
}

TEST(MaterialTailReferences, NoAllocationFinalRescanHandlesVariantsPointersAndProspectiveBindingPublication) {
  Fixture f;f.variant(0);f.pointer("/materials/0/extensions/FUTURE_long_property_name/value");
  f.graph.setMaterialIndex(f.leaf,6);
  const auto command=PlanMaterialBindings(f.graph,f.context(),std::array{MaterialBindingRequest{f.leaf,6,2}});
  auto bindings=PrepareMaterialBindings(f.graph,f.context(),command);const auto audit=AuditMaterialTailRemoval(f.view(),&bindings);
  const auto view=f.view();bool before{},published{},after{},wrongPhase{};
  material_publication_test::AllocationGuard guard(true);
  before=CanRemoveMaterialTail(view,audit,MaterialTailAuditPhase::BeforeBindings);
  published=PublishMaterialBindings(f.graph,bindings,f.context());
  after=CanRemoveMaterialTail(view,audit,MaterialTailAuditPhase::AfterBindings);
  wrongPhase=!CanRemoveMaterialTail(view,audit,MaterialTailAuditPhase::BeforeBindings);
  guard.disable();
  EXPECT_TRUE(before);EXPECT_TRUE(published);EXPECT_TRUE(after);EXPECT_TRUE(wrongPhase);EXPECT_EQ(guard.allocations(),0u);
}
} // namespace
