#include <Container/utility/MaterialTailReferences.h>

#include "../renderer/scene/material_publication_allocations.h"

#include <array>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace container::scene;
using namespace container::geometry::gltf;
using V=tinygltf::Value;using O=V::Object;using A=V::Array;
struct Fixture {
  tinygltf::Model authored,evaluated;
  SceneGraph graph;
  MaterialVariantLibrary variants;
  std::vector<uint32_t> mapping{4,1,6,3},protectedIds{0,2};
  uint32_t group{},leaf{};
  Fixture() {
    authored.asset.version="2.0";authored.materials.resize(4);
    for(size_t i=0;i<4;++i) authored.materials[i].name="Material "+std::to_string(i);
    tinygltf::Primitive primitive;primitive.material=0;
    authored.meshes.resize(1);authored.meshes[0].primitives.push_back(primitive);sync();
    group=graph.createNode(glm::mat4(1),1,false,SceneGraph::kInvalidNode,"Group placeholder");
    leaf=graph.createNode(glm::mat4(1),4,true,0,"Surviving leaf");graph.setParent(leaf,group);graph.updateWorldTransforms();
  }
  void sync() {evaluated=authored;variants=ParseMaterialVariants(authored);}
  MaterialBindingContext context() const {return {93,mapping,8,1};}
  MaterialReferenceView view() const {return {authored,evaluated,graph,variants,context(),1,1,protectedIds};}
  void variant(int material) {
    authored.extensionsUsed={"KHR_materials_variants"};
    authored.extensions["KHR_materials_variants"]=V(O{{"variants",V(A{V(O{{"name",V(std::string("Option"))}})})}});
    authored.meshes[0].primitives[0].extensions["KHR_materials_variants"]=V(O{{"mappings",V(A{
        V(O{{"material",V(material)},{"variants",V(A{V(0)})}})})}});sync();
  }
  void pointer(std::string text) {
    authored.extensionsUsed.push_back("KHR_animation_pointer");authored.animations.resize(1);
    tinygltf::AnimationChannel channel;channel.target_node=-1;channel.target_path="pointer";
    channel.target_extensions["KHR_animation_pointer"]=V(O{{"pointer",V(std::move(text))}});
    authored.animations[0].channels.push_back(channel);evaluated=authored;
  }
};

TEST(MaterialReferences, MiddleSourceAndRuntimeRowsUseExplicitNonidentityPairWithoutCompaction) {
  Fixture f;const auto before=f.authored;const auto revision=f.graph.revision();
  const auto audit=AuditUnusedMaterialReferences(f.view());ASSERT_TRUE(audit);
  const auto view=f.view();bool beforeBindings{},afterBindings{};
  material_publication_test::AllocationGuard guard(true);
  beforeBindings=CanRetireUnusedMaterial(view,audit,MaterialReferencePhase::BeforeBindings);
  afterBindings=CanRetireUnusedMaterial(view,audit,MaterialReferencePhase::AfterBindings);guard.disable();
  EXPECT_TRUE(beforeBindings);EXPECT_TRUE(afterBindings);EXPECT_EQ(guard.allocations(),0u);
  EXPECT_EQ(f.authored,before);EXPECT_EQ(f.graph.revision(),revision);
  const MaterialTailReferenceView tail{f.authored,f.evaluated,f.graph,f.variants,f.context(),1,1,f.protectedIds};
  EXPECT_THROW(AuditMaterialTailRemoval(tail),std::invalid_argument);
  EXPECT_FALSE(CanRemoveMaterialTail(tail,audit,MaterialTailAuditPhase::BeforeBindings));
}

TEST(MaterialReferences, EveryRenderableRowBlocksIncludingHiddenInactiveAndRetiredRows) {
  for(const int state:{0,1,2,3}) {
    Fixture f;f.graph.setMaterialIndex(f.leaf,1);
    if(state==1) f.graph.setVisible(f.leaf,false);
    if(state==2) f.graph.setActiveRoots(std::array<uint32_t,0>{});
    if(state==3) f.graph.getNode(f.leaf)->retired=true;
    EXPECT_THROW(AuditUnusedMaterialReferences(f.view()),std::invalid_argument);
  }
}

TEST(MaterialReferences, BothSourceSnapshotsAndRawOrParsedVariantsBlockMiddleRows) {
  for(const bool authored:{false,true}) {
    Fixture f;(authored?f.authored:f.evaluated).meshes[0].primitives[0].material=1;
    EXPECT_THROW(AuditUnusedMaterialReferences(f.view()),std::invalid_argument);
  }
  Fixture f;f.variant(1);EXPECT_THROW(AuditUnusedMaterialReferences(f.view()),std::invalid_argument);
  const auto stale=f.variants;f.variant(0);
  const MaterialReferenceView view{f.authored,f.evaluated,f.graph,stale,f.context(),1,1,f.protectedIds};
  EXPECT_THROW(AuditUnusedMaterialReferences(view),std::invalid_argument);
  const auto audit=AuditUnusedMaterialReferences(f.view());
  f.evaluated.meshes[0].primitives[0].extensions.at("KHR_materials_variants").Get<O>().at("mappings").Get<A>()[0].Get<O>().at("material")=V(1);
  EXPECT_FALSE(CanRetireUnusedMaterial(f.view(),audit,MaterialReferencePhase::BeforeBindings));
}

TEST(MaterialReferences, TypedRawLodAndAnyMaterialPointerSuffixRemainIncomingReferences) {
  for(const int kind:{0,1,2,3}) {
    Fixture f;
    if(kind==0) {f.authored.materials[3].lods={1};f.sync();}
    if(kind==1) {f.authored.extensionsUsed={"MSFT_lod"};f.authored.materials[0].extensions["MSFT_lod"]=V(O{{"ids",V(A{V(1)})}});f.sync();}
    if(kind==2) f.pointer("/materials/1/extensions/FUTURE_property/value~1suffix");
    if(kind==3) f.pointer("/materials/1");
    EXPECT_THROW(AuditUnusedMaterialReferences(f.view()),std::invalid_argument);
  }
  Fixture f;f.authored.materials[1].lods={0,1,3};
  f.authored.materials[1].extensions["VENDOR_outgoing"]=V(O{{"material",V(1)}});f.sync();
  EXPECT_TRUE(AuditUnusedMaterialReferences(f.view()));
}

TEST(MaterialReferences, PairOwnershipProtectedDefaultsAliasesAndForeignContextReject) {
  Fixture f;f.protectedIds.push_back(1);EXPECT_THROW(AuditUnusedMaterialReferences(f.view()),std::invalid_argument);
  f.protectedIds={0,2};f.mapping[2]=1;EXPECT_THROW(AuditUnusedMaterialReferences(f.view()),std::invalid_argument);
  f.mapping[2]=6;auto view=f.view();view.runtimeMaterial=3;EXPECT_THROW(AuditUnusedMaterialReferences(view),std::invalid_argument);
  auto outside=f.view();outside.sourceMaterial=4;EXPECT_THROW(AuditUnusedMaterialReferences(outside),std::invalid_argument);
}

TEST(MaterialReferences, ProjectedMiddleBindingChangeIsOwnedAndRecheckedAcrossBothPhases) {
  Fixture f;f.graph.setMaterialIndex(f.leaf,1);
  const auto command=PlanMaterialBindings(f.graph,f.context(),std::array{MaterialBindingRequest{f.leaf,1,4}});
  auto change=PrepareMaterialBindings(f.graph,f.context(),command);
  const auto audit=AuditUnusedMaterialReferences(f.view(),&change);
  ASSERT_TRUE(CanRetireUnusedMaterial(f.view(),audit,MaterialReferencePhase::BeforeBindings));
  EXPECT_FALSE(CanRetireUnusedMaterial(f.view(),audit,MaterialReferencePhase::AfterBindings));
  ASSERT_TRUE(PublishMaterialBindings(f.graph,change,f.context()));change={};
  EXPECT_TRUE(CanRetireUnusedMaterial(f.view(),audit,MaterialReferencePhase::AfterBindings));
  f.evaluated.materials[2].lods={1};
  EXPECT_FALSE(CanRetireUnusedMaterial(f.view(),audit,MaterialReferencePhase::AfterBindings));
}

TEST(MaterialReferences, UnknownNormativeReferencesRejectButOpaqueExtrasKeepTheirValues) {
  Fixture f;f.authored.extras=V(O{{"material",V(1)},{"extensions",V(O{{"VENDOR_unknown",V(O{{"index",V(1)}})}})}});f.sync();
  EXPECT_TRUE(AuditUnusedMaterialReferences(f.view()));
  f.authored.extensionsUsed={"VENDOR_unknown"};f.authored.nodes.resize(1);
  f.authored.nodes[0].extensions["VENDOR_unknown"]=V(O{{"material",V(1)}});f.sync();
  EXPECT_THROW(AuditUnusedMaterialReferences(f.view()),std::invalid_argument);
}

TEST(MaterialReferences, ArbitraryRowProofStorageAndRescanWorkRemainBounded) {
  Fixture f;
  for(const int kind:{0,1,2,3}) {
    MaterialReferenceLimits limits;
    if(kind==0) limits.maxOwnedBytes=1;
    if(kind==1) limits.maxTraversalSteps=1;
    if(kind==2) limits.maxMetadataBytes=1;
    if(kind==3) limits.maxReferences=1;
    EXPECT_THROW(AuditUnusedMaterialReferences(f.view(),nullptr,limits),std::invalid_argument);
  }
  const auto audit=AuditUnusedMaterialReferences(f.view());f.mapping[3]=7;
  EXPECT_FALSE(CanRetireUnusedMaterial(f.view(),audit,MaterialReferencePhase::BeforeBindings));
}

TEST(MaterialReferences, RuntimeOnlyRowsHaveNoFabricatedSourceIdentityAndRejectAnySourceOwner) {
  Fixture f;auto view=f.view();view.sourceMaterial=UINT32_MAX;view.runtimeMaterial=7;
  const auto audit=AuditUnusedMaterialReferences(view);EXPECT_TRUE(CanRetireUnusedMaterial(view,audit,MaterialReferencePhase::BeforeBindings));
  f.mapping[2]=7;EXPECT_THROW(AuditUnusedMaterialReferences(view),std::invalid_argument);f.mapping[2]=6;
  f.graph.setMaterialIndex(f.leaf,7);EXPECT_THROW(AuditUnusedMaterialReferences(view),std::invalid_argument);
  tinygltf::Model a,e;SceneGraph graph;const auto variants=ParseMaterialVariants(a);
  const MaterialBindingContext context{91,{},3,0};const std::array<uint32_t,1> protectedIds{0};
  const MaterialReferenceView procedural{a,e,graph,variants,context,UINT32_MAX,1,protectedIds};
  EXPECT_TRUE(AuditUnusedMaterialReferences(procedural));
}

TEST(MaterialReferences, RegisteredUnsupportedAlternateImageSourceIsPreservedAsNonmaterialMetadata) {
  Fixture f;f.authored.extensionsUsed={"EXT_texture_avif"};f.authored.images.resize(1);f.authored.textures.resize(1);
  f.authored.textures[0].extensions["EXT_texture_avif"]=V(O{{"source",V(0)}});f.sync();
  EXPECT_TRUE(AuditUnusedMaterialReferences(f.view()));
}
} // namespace
