#include "Container/utility/TextureResourceLifecycle.h"
#include <gtest/gtest.h>
#include "../renderer/scene/material_publication_allocations.h"

#include <algorithm>
#include <array>
#include <memory>
#include <optional>
#include <stdexcept>
#include <vector>

namespace {
using namespace container::scene;
using namespace container::util;
using Kind=ResourceKind;
using Domain=ResourceDomainKind;
using Pins=ResourcePinClass;
PreparedTextureResourceLifecycle sealedPrepare(const TextureResourceLifecycleGenerationPtr& before,
    const TextureResourceLifecycleView& view,const TextureResourceLifecycleLimits& limits={}) {
  auto value=PrepareTextureResourceLifecycle(before,view,limits);value.seal();return value;
}
PreparedTextureResourceLifecycle sealedReconcile(const TextureResourceLifecycleGenerationPtr& before,
    const TextureResourceLifecycleView& view,const TextureResourceLifecycleLimits& limits={}) {
  auto value=ReconcileTextureResourceLifecycle(before,view,limits);value.seal();return value;
}
PreparedTextureResourceLifecycle sealedReplay(const TextureResourceLifecycleGenerationPtr& before,
    const TextureResourceLifecycleView& view,const TextureResourceLifecycleCommand& command,bool forward,
    const TextureResourceLifecycleLimits& limits={}) {
  auto value=PrepareTextureResourceLifecycleReplay(before,view,command,forward,limits);value.seal();return value;
}
using container::material::TextureManager;
using container::material::TextureResource;
using container::material::RuntimeTextureRetirementTarget;
TextureResourceLifecycleView withRuntime(TextureResourceLifecycleView view,const TextureManager& runtime) {
  return {view.archive,view.evaluated,runtime,view.archiveToEvaluated,view.archiveTextureIdentities};
}

struct Fixture {
  tinygltf::Model archive,evaluated;
  TextureManager runtime;
  TextureResourceLifecycleGenerationPtr generation;
  Fixture() {
    tinygltf::Image image;image.name="Base image";image.uri="base.png";image.image={1,2,3,4};
    archive.images.push_back(image);archive.samplers.emplace_back();
    tinygltf::Texture texture;texture.name="Base texture";texture.source=0;texture.sampler=0;archive.textures.push_back(texture);
    evaluated=archive;TextureResource row;row.name="Base runtime";row.contentKey="base";runtime.registerTexture(row);
  }
  TextureResourceLifecycleView view() const {
    return {{archive.images,archive.textures,archive.samplers,archive.bufferViews,archive.buffers},
      {evaluated.images,evaluated.textures,evaluated.samplers,evaluated.bufferViews,evaluated.buffers},runtime};
  }
  void publish() {auto prepared=sealedPrepare(generation,view());ASSERT_TRUE(ExchangeTextureResourceLifecycle(prepared,generation,view()));}
  void append(const char* suffix="New") {
    auto image=archive.images.front();image.name=suffix;image.uri=std::string(suffix)+".png";archive.images.push_back(image);
    archive.samplers.emplace_back();tinygltf::Texture texture;texture.name=suffix;texture.source=int(archive.images.size()-1);
    texture.sampler=int(archive.samplers.size()-1);archive.textures.push_back(texture);evaluated=archive;
    TextureResource row;row.name=suffix;row.contentKey=suffix;runtime.registerTexture(row);
  }
  void trim() {archive.images.resize(1);archive.textures.resize(1);archive.samplers.resize(1);evaluated=archive;runtime=runtime.copyPrefix(1);}
};
struct Roles {
  std::vector<uint32_t> aColor,aData,eColor,eData;
  std::vector<int32_t> mapping;
  std::vector<std::shared_ptr<const uint8_t>> cookies;
  void sync(const Fixture& f,uint32_t runtime=0) {
    aColor.assign(f.archive.textures.size(),runtime);aData.assign(f.archive.textures.size(),UINT32_MAX);
    eColor.assign(f.evaluated.textures.size(),runtime);eData.assign(f.evaluated.textures.size(),UINT32_MAX);
    mapping.resize(f.archive.textures.size());for(size_t i=0;i<mapping.size();++i) mapping[i]=i<f.evaluated.textures.size()?int32_t(i):-1;
    cookies.resize(f.archive.textures.size());for(auto& cookie:cookies) if(!cookie) cookie=std::make_shared<const uint8_t>(0);
  }
  TextureResourceLifecycleView view(const Fixture& f) const {
    auto result=f.view();result.archive.colorResources=aColor;result.archive.dataResources=aData;result.archive.rolesKnown=true;
    result.evaluated.colorResources=eColor;result.evaluated.dataResources=eData;result.evaluated.rolesKnown=true;
    result.archiveToEvaluated=mapping;result.archiveTextureIdentities=cookies;return result;
  }
};
struct SourcePhases {
  std::vector<uint8_t> aImages,aTextures,aSamplers,eImages,eTextures,eSamplers;
  void sync(const Fixture& f) {
    aImages.assign(f.archive.images.size(),1);aTextures.assign(f.archive.textures.size(),1);aSamplers.assign(f.archive.samplers.size(),1);
    eImages.assign(f.evaluated.images.size(),1);eTextures.assign(f.evaluated.textures.size(),1);eSamplers.assign(f.evaluated.samplers.size(),1);
  }
  TextureResourceLifecycleView apply(TextureResourceLifecycleView view) const {
    view.archive.imageActive=aImages;view.archive.textureActive=aTextures;view.archive.samplerActive=aSamplers;
    view.evaluated.imageActive=eImages;view.evaluated.textureActive=eTextures;view.evaluated.samplerActive=eSamplers;return view;
  }
};
const ResourceInventoryRow* find(const ResourceCatalogSnapshot& snapshot,const ResourceHandle& handle) {
  for(const auto& row:snapshot.rows()) if(row.resource==handle) return &row;return nullptr;
}

TEST(TextureResourceLifecycle, BootstrapStagesIndependentNamespacesAndPublishesWithoutAllocations) {
  Fixture f;auto prepared=sealedPrepare({},f.view());
  const std::array selected{TextureResourceSelection{Domain::Archive,Kind::Texture,0}};
  auto future=prepared.select(selected,Pins::Preparation);ASSERT_TRUE(future);EXPECT_TRUE(future->valid());EXPECT_FALSE(future->active());
  ASSERT_EQ(future->resources().size(),3u);EXPECT_FALSE(prepared.next()->valid());
  bool ready{},published{};size_t attempts{};
  {
    material_publication_test::AllocationGuard guard(true);
    ready=CanExchangeTextureResourceLifecycle(prepared,f.generation,f.view());
    published=ExchangeTextureResourceLifecycle(prepared,f.generation,f.view());attempts=guard.allocations();
  }
  EXPECT_TRUE(ready);EXPECT_TRUE(published);EXPECT_EQ(attempts,0u);ASSERT_TRUE(f.generation);
  EXPECT_TRUE(f.generation->valid());EXPECT_TRUE(f.generation->matches(f.view()));EXPECT_TRUE(future->active());
  EXPECT_FALSE(f.generation->dependencyAuthorityComplete());
  const auto a=f.generation->resource({Domain::Archive,Kind::Image,0});
  const auto e=f.generation->resource({Domain::Evaluated,Kind::Image,0});
  EXPECT_NE(a,e);EXPECT_NE(a.domain(),e.domain());EXPECT_EQ(f.generation->rowCount(Domain::Runtime,Kind::RuntimeTexture),1u);
  EXPECT_FALSE(ExchangeTextureResourceLifecycle(prepared,f.generation,f.view()));
}

TEST(TextureResourceLifecycle, SelectedTextureOwnsEveryImageAlternativeAndSamplerOnly) {
  Fixture f;
  for(int i=1;i<4;++i) {auto image=f.archive.images.front();image.name=std::to_string(i);f.archive.images.push_back(image);}
  for(const auto& [name,index]:std::array<std::pair<const char*,int>,3>{{{"KHR_texture_basisu",1},{"EXT_texture_webp",2},{"MSFT_texture_dds",3}}})
    f.archive.textures[0].extensions[name]=tinygltf::Value(tinygltf::Value::Object{{"source",tinygltf::Value(index)}});
  // An unrelated large image must not be retained by this selected endpoint.
  auto unrelated=f.archive.images.front();unrelated.image.resize(1'000'000);f.archive.images.push_back(std::move(unrelated));
  f.evaluated=f.archive;f.publish();
  const std::array selection{TextureResourceSelection{Domain::Archive,Kind::Texture,0}};
  auto selected=RetainTextureResourceReferences(f.generation,selection,Pins::History);
  ASSERT_TRUE(selected);EXPECT_TRUE(selected->complete());EXPECT_TRUE(selected->active());EXPECT_EQ(selected->resources().size(),6u);
  EXPECT_LT(selected->ownedBytes(),100'000u);
  for(const auto& resource:selected->resources()) EXPECT_EQ(resource.domain().kind(),Domain::Archive);
}

TEST(TextureResourceLifecycle, OpaqueControlsExistEvenWithoutKnownRowsAndUnionsRetainControlsOnly) {
  Fixture f;f.publish();
  auto opaque=RetainTextureResourceReferences(f.generation,{},Pins::History,64ull*1024*1024,true);
  ASSERT_TRUE(opaque);EXPECT_TRUE(opaque->valid());EXPECT_FALSE(opaque->complete());EXPECT_TRUE(opaque->resources().empty());
  EXPECT_EQ(f.generation->opaqueReferenceOwnerCount(),1u);
  TextureResourceReferenceOwnerPtr combined;
  {const std::array inputs{opaque,opaque};combined=CombineTextureResourceReferences(inputs);}
  EXPECT_EQ(f.generation->opaqueReferenceOwnerCount(),1u);opaque.reset();EXPECT_FALSE(combined->complete());
  EXPECT_EQ(f.generation->opaqueReferenceOwnerCount(),1u);
  combined.reset();EXPECT_EQ(f.generation->opaqueReferenceOwnerCount(),0u);
  auto pending=sealedPrepare(f.generation,f.view());
  auto escaped=pending.select({},Pins::Preparation,64ull*1024*1024,true);
  EXPECT_EQ(f.generation->opaqueReferenceOwnerCount(),1u);pending={};
  EXPECT_FALSE(escaped->valid());EXPECT_EQ(f.generation->opaqueReferenceOwnerCount(),0u);
}

TEST(TextureResourceLifecycle, UnknownTextureSchemaRetainsKnownClosureAndBlocksThroughSelectedOwner) {
  Fixture f;f.archive.textures[0].extensions["VENDOR_opaque_image"]=tinygltf::Value(tinygltf::Value::Object{{"source",tinygltf::Value(0)}});
  f.evaluated=f.archive;f.publish();const std::array selection{TextureResourceSelection{Domain::Archive,Kind::Texture,0}};
  auto selected=RetainTextureResourceReferences(f.generation,selection,Pins::History);
  EXPECT_TRUE(selected->valid());EXPECT_FALSE(selected->complete());EXPECT_EQ(selected->resources().size(),3u);
  EXPECT_EQ(f.generation->opaqueReferenceOwnerCount(),3u);selected.reset();EXPECT_EQ(f.generation->opaqueReferenceOwnerCount(),2u);
}

TEST(TextureResourceLifecycle, SelectedImagesSamplersAndImageBufferViewsPropagateOpaqueNormativeMetadata) {
  for(unsigned variant=0;variant<3;++variant) {
    Fixture f;
    const auto unknown=tinygltf::Value(tinygltf::Value::Object{{"source",tinygltf::Value(0)}});
    if(variant==0) f.archive.images[0].extensions["VENDOR_image_reference"]=unknown;
    else if(variant==1) f.archive.samplers[0].extensions["VENDOR_sampler_reference"]=unknown;
    else {
      f.archive.buffers.emplace_back();f.archive.buffers[0].data={1,2,3,4};
      tinygltf::BufferView view;view.buffer=0;view.byteLength=4;view.extensions["VENDOR_buffer_reference"]=unknown;
      f.archive.bufferViews.push_back(view);f.archive.images[0].bufferView=0;
    }
    f.evaluated=f.archive;f.publish();
    const std::array texture{TextureResourceSelection{Domain::Archive,Kind::Texture,0}};
    auto closure=RetainTextureResourceReferences(f.generation,texture,Pins::History);
    EXPECT_TRUE(closure->valid());EXPECT_FALSE(closure->complete());EXPECT_EQ(closure->resources().size(),3u);
    const std::array direct{TextureResourceSelection{Domain::Archive,variant==1?Kind::Sampler:Kind::Image,0}};
    auto row=RetainTextureResourceReferences(f.generation,direct,Pins::History);
    EXPECT_TRUE(row->valid());EXPECT_FALSE(row->complete());EXPECT_EQ(row->resources().size(),1u);
    EXPECT_EQ(f.generation->opaqueReferenceOwnerCount(),4u);
    closure.reset();EXPECT_EQ(f.generation->opaqueReferenceOwnerCount(),3u);
    row.reset();EXPECT_EQ(f.generation->opaqueReferenceOwnerCount(),2u);
  }
  Fixture annotations;const auto extra=tinygltf::Value(tinygltf::Value::Object{{"source",tinygltf::Value(999)}});
  annotations.archive.images[0].extras=extra;annotations.archive.samplers[0].extras=extra;
  annotations.evaluated=annotations.archive;annotations.publish();
  const std::array texture{TextureResourceSelection{Domain::Archive,Kind::Texture,0}};
  auto selected=RetainTextureResourceReferences(annotations.generation,texture,Pins::History);
  EXPECT_TRUE(selected->complete());EXPECT_EQ(annotations.generation->opaqueReferenceOwnerCount(),0u);
}

TEST(TextureResourceLifecycle, AbandonedAppendCancelsEscapedPinsWithoutChangingCurrentMaps) {
  Fixture f;f.publish();const auto old=f.generation;
  f.append();TextureResourceReferenceOwnerPtr escaped;ResourceHandle candidate;
  {
    auto prepared=sealedPrepare(f.generation,f.view());candidate=prepared.next()->resource({Domain::Archive,Kind::Texture,1});
    const std::array selection{TextureResourceSelection{Domain::Archive,Kind::Texture,1}};
    escaped=prepared.select(selection,Pins::History);EXPECT_TRUE(escaped->valid());EXPECT_FALSE(escaped->active());
  }
  EXPECT_FALSE(escaped->valid());EXPECT_EQ(f.generation,old);EXPECT_EQ(f.generation->rowCount(Domain::Archive,Kind::Texture),1u);
  auto retry=sealedPrepare(f.generation,f.view());const auto replacement=retry.next()->resource({Domain::Archive,Kind::Texture,1});
  EXPECT_NE(candidate,replacement);EXPECT_TRUE(ExchangeTextureResourceLifecycle(retry,f.generation,f.view()));
}

TEST(TextureResourceLifecycle, OwnedTailUndoRedoPreservesIncarnationsAndBranchReuseNeverRecyclesThem) {
  Fixture f;f.publish();const auto base=f.generation->resource({Domain::Archive,Kind::Texture,0});const auto baseBytes=f.generation->ownedBytes();
  f.append();auto append=sealedPrepare(f.generation,f.view());auto command=append.command();ASSERT_TRUE(command);
  ASSERT_TRUE(ExchangeTextureResourceLifecycle(append,f.generation,f.view()));
  const auto original=f.generation->resource({Domain::Archive,Kind::Texture,1});auto retained=command.references();
  ASSERT_TRUE(retained);EXPECT_TRUE(retained->valid());EXPECT_TRUE(retained->active());
  f.trim();auto undo=sealedReplay(f.generation,f.view(),command,false);
  auto future=undo.selectAll(Pins::Preparation);bool published{};size_t attempts{};
  {material_publication_test::AllocationGuard guard(true);published=ExchangeTextureResourceLifecycle(undo,f.generation,f.view());attempts=guard.allocations();}
  EXPECT_TRUE(published);EXPECT_EQ(attempts,0u);EXPECT_TRUE(retained->valid());EXPECT_FALSE(retained->active());
  EXPECT_TRUE(future->active());EXPECT_EQ(f.generation->resource({Domain::Archive,Kind::Texture,0}),base);
  EXPECT_EQ(f.generation->ownedBytes(),baseBytes); // Unrelated history/high waters do not change resident accounting.
  f.append();auto redo=sealedReplay(f.generation,f.view(),command,true);
  auto futureTail=redo.selectAll(Pins::Preparation);EXPECT_TRUE(futureTail->valid());EXPECT_FALSE(futureTail->active());
  {material_publication_test::AllocationGuard guard(true);published=ExchangeTextureResourceLifecycle(redo,f.generation,f.view());attempts=guard.allocations();}
  EXPECT_TRUE(published);EXPECT_EQ(attempts,0u);EXPECT_EQ(f.generation->resource({Domain::Archive,Kind::Texture,1}),original);
  EXPECT_TRUE(retained->active());futureTail.reset();future.reset();
  f.trim();auto undoAgain=sealedReplay(f.generation,f.view(),command,false);
  ASSERT_TRUE(ExchangeTextureResourceLifecycle(undoAgain,f.generation,f.view()));
  f.append("Branch");auto branch=sealedPrepare(f.generation,f.view());
  const auto branched=branch.next()->resource({Domain::Archive,Kind::Texture,1});EXPECT_NE(branched,original);EXPECT_GT(branched.slot(),original.slot());
  ASSERT_TRUE(ExchangeTextureResourceLifecycle(branch,f.generation,f.view()));
  EXPECT_THROW(sealedReplay(f.generation,f.view(),command,true),std::invalid_argument);
}

TEST(TextureResourceLifecycle, ExactReuseCommandsHaveBoundedEmptyHistoryAndReplayEndpoints) {
  Fixture f;f.publish();auto prepared=sealedPrepare(f.generation,f.view());auto command=prepared.command();
  ASSERT_TRUE(command);ASSERT_TRUE(command.references());EXPECT_TRUE(command.references()->resources().empty());
  ASSERT_TRUE(ExchangeTextureResourceLifecycle(prepared,f.generation,f.view()));
  auto undo=sealedReplay(f.generation,f.view(),command,false);ASSERT_TRUE(ExchangeTextureResourceLifecycle(undo,f.generation,f.view()));
  auto redo=sealedReplay(f.generation,f.view(),command,true);ASSERT_TRUE(ExchangeTextureResourceLifecycle(redo,f.generation,f.view()));
  EXPECT_TRUE(f.generation->matches(f.view()));EXPECT_LT(command.ownedBytes(),20'000u);
}

TEST(TextureResourceLifecycle, RuntimeTombstonesRetainIdentitiesAndFullPreparationSkipsInactiveRows) {
  Fixture f;ASSERT_TRUE(f.runtime.retireTexture(0));f.publish();const auto retired=f.generation->resource({Domain::Runtime,Kind::RuntimeTexture,0});
  ASSERT_TRUE(retired);auto snapshot=f.generation->inventory();ASSERT_NE(find(snapshot,retired),nullptr);EXPECT_TRUE(find(snapshot,retired)->retired);
  auto all=RetainAllTextureResourceReferences(f.generation,Pins::Preparation);EXPECT_TRUE(all->active());
  for(const auto& handle:all->resources()) EXPECT_NE(handle,retired);
  const std::array selection{TextureResourceSelection{Domain::Runtime,Kind::RuntimeTexture,0}};
  EXPECT_THROW((void)RetainTextureResourceReferences(f.generation,selection,Pins::Preparation),std::invalid_argument);
  auto unchanged=sealedPrepare(f.generation,f.view());auto future=unchanged.selectAll(Pins::Preparation);
  ASSERT_TRUE(ExchangeTextureResourceLifecycle(unchanged,f.generation,f.view()));EXPECT_TRUE(future->active());
}

TEST(TextureResourceLifecycle, MaintenanceRetirementKeepsDefaultPreparationBlockAndExplicitInvalidation) {
  Fixture f;f.publish();const std::array selection{TextureResourceSelection{Domain::Runtime,Kind::RuntimeTexture,0}};
  auto preparation=RetainTextureResourceReferences(f.generation,selection,Pins::Preparation);
  ASSERT_TRUE(f.runtime.retireTexture(0));EXPECT_THROW(sealedPrepare(f.generation,f.view()),std::invalid_argument);
  TextureResourceLifecycleLimits limits;limits.retirementPolicy=ResourceRetirementPolicy::InvalidatePreparations;
  auto retired=sealedPrepare(f.generation,f.view(),limits);EXPECT_FALSE(retired.command());
  ASSERT_TRUE(ExchangeTextureResourceLifecycle(retired,f.generation,f.view()));EXPECT_TRUE(preparation->valid());EXPECT_FALSE(preparation->resourceDependenciesActive());
  EXPECT_TRUE(f.generation->matches(f.view()));
}

TEST(TextureResourceLifecycle, MatchesChecksRawMetadataAndExactImageByteSlicesWithoutCopyingWholeBuffers) {
  Fixture f;f.archive.buffers.emplace_back();f.archive.buffers[0].data.resize(1'000'000,9);
  tinygltf::BufferView view;view.buffer=0;view.byteOffset=20;view.byteLength=4;f.archive.bufferViews.push_back(view);
  f.archive.images[0].bufferView=0;f.archive.images[0].bits=8;f.archive.images[0].extensions_json_string="{\"x\":1}";
  f.evaluated=f.archive;f.publish();const std::array selection{TextureResourceSelection{Domain::Archive,Kind::Image,0}};
  auto selected=RetainTextureResourceReferences(f.generation,selection,Pins::History);EXPECT_LT(selected->ownedBytes(),20'000u);
  f.archive.buffers[0].data[10]=0;EXPECT_TRUE(f.generation->matches(f.view()));
  f.archive.buffers[0].data[20]=0;EXPECT_FALSE(f.generation->matches(f.view()));f.archive.buffers[0].data[20]=9;
  f.archive.images[0].bits=16;EXPECT_FALSE(f.generation->matches(f.view()));f.archive.images[0].bits=8;
  f.archive.images[0].extensions_json_string="{\"x\":2}";EXPECT_FALSE(f.generation->matches(f.view()));
  EXPECT_THROW(sealedPrepare(f.generation,f.view()),std::invalid_argument);
}

TEST(TextureResourceLifecycle, StaleWrongViewForeignAndBudgetFailuresPreservePublication) {
  Fixture f;f.publish();auto first=sealedPrepare(f.generation,f.view());auto second=sealedPrepare(f.generation,f.view());
  ASSERT_TRUE(ExchangeTextureResourceLifecycle(first,f.generation,f.view()));
  bool ready{},published{};size_t attempts{};
  {material_publication_test::AllocationGuard guard(true);ready=CanExchangeTextureResourceLifecycle(second,f.generation,f.view());published=ExchangeTextureResourceLifecycle(second,f.generation,f.view());attempts=guard.allocations();}
  EXPECT_FALSE(ready);EXPECT_FALSE(published);EXPECT_EQ(attempts,0u);
  auto prepared=sealedPrepare(f.generation,f.view());f.archive.images[0].uri="mutated.png";
  {material_publication_test::AllocationGuard guard(true);ready=CanExchangeTextureResourceLifecycle(prepared,f.generation,f.view());attempts=guard.allocations();}
  EXPECT_FALSE(ready);EXPECT_EQ(attempts,0u);f.archive.images[0].uri="base.png";
  Fixture foreign;foreign.publish();EXPECT_FALSE(CanExchangeTextureResourceLifecycle(prepared,foreign.generation,f.view()));
  TextureResourceLifecycleLimits limits;limits.maxRows=1;EXPECT_THROW(sealedPrepare({},f.view(),limits),std::length_error);
  limits={};limits.maxOwnedBytes=32;EXPECT_THROW(sealedPrepare({},f.view(),limits),std::length_error);
  const std::array selection{TextureResourceSelection{Domain::Archive,Kind::Texture,0}};
  EXPECT_THROW((void)RetainTextureResourceReferences(f.generation,selection,Pins::History,32),std::length_error);
}

TEST(TextureResourceLifecycle, AddedMaterialHistoryPinCanRetainRetiredMaterialWhileResourceDependenciesStayActive) {
  Fixture f;f.publish();const std::array selection{TextureResourceSelection{Domain::Archive,Kind::Texture,0}};
  auto selected=RetainTextureResourceReferences(f.generation,selection,Pins::History);
  ResourceLifecycleCatalog materials;const auto domain=materials.createDomain(Domain::Runtime);const auto material=materials.issue(domain,Kind::Material,0);
  const std::array materialRows{material};auto pin=materials.pin(materialRows,Pins::History,"Material history endpoint");
  auto joined=AddTextureResourceReferencePin(selected,std::move(pin));EXPECT_TRUE(joined->active());
  auto retirement=materials.prepareRetirement(materialRows,"Retire material only");ASSERT_TRUE(materials.exchange(retirement));
  EXPECT_TRUE(joined->valid());EXPECT_FALSE(joined->active());EXPECT_TRUE(joined->resourceDependenciesActive());
}

TEST(TextureResourceLifecycle, SelectedOwnersRejectFreshForeignMapsEvenWithIdenticalNumericAddresses) {
  Fixture f;f.publish();Fixture foreign;foreign.publish();const std::array selection{TextureResourceSelection{Domain::Archive,Kind::Texture,0}};
  auto selected=RetainTextureResourceReferences(f.generation,selection,Pins::History);
  auto empty=RetainTextureResourceReferences(f.generation,{},Pins::History);
  bool exact{},wrong{},emptyWrong{};size_t attempts{};
  {
    material_publication_test::AllocationGuard guard(true);
    exact=selected->resourceDependenciesMatch(f.generation);wrong=selected->resourceDependenciesMatch(foreign.generation);
    emptyWrong=empty->resourceDependenciesMatch(foreign.generation);attempts=guard.allocations();
  }
  EXPECT_TRUE(exact);EXPECT_FALSE(wrong);EXPECT_FALSE(emptyWrong);EXPECT_EQ(attempts,0u);
}

TEST(TextureResourceLifecycle, AllocationFailurePreservesExistingPendingOwnerAndCurrentGeneration) {
  Fixture f;f.publish();const auto current=f.generation;f.append();bool failed{};size_t attempts{};
  {
    material_publication_test::AllocationGuard guard(true);
    try {(void)sealedPrepare(f.generation,f.view());} catch(const std::bad_alloc&) {failed=true;}
    attempts=guard.allocations();
  }
  EXPECT_TRUE(failed);EXPECT_GT(attempts,0u);EXPECT_EQ(f.generation,current);EXPECT_TRUE(f.generation->valid());
  auto prepared=sealedPrepare(f.generation,f.view());const std::array selection{TextureResourceSelection{Domain::Archive,Kind::Texture,1}};
  auto existing=prepared.select(selection,Pins::History);failed=false;
  {
    material_publication_test::AllocationGuard guard(true);
    try {(void)prepared.select(selection,Pins::GpuReader);} catch(const std::bad_alloc&) {failed=true;}
    attempts=guard.allocations();
  }
  EXPECT_TRUE(failed);EXPECT_GT(attempts,0u);EXPECT_TRUE(existing->valid());EXPECT_FALSE(existing->active());
  bool published{};
  {material_publication_test::AllocationGuard guard(true);published=ExchangeTextureResourceLifecycle(prepared,f.generation,f.view());attempts=guard.allocations();}
  EXPECT_TRUE(published);EXPECT_EQ(attempts,0u);EXPECT_TRUE(existing->active());
}

TEST(TextureResourceLifecycle, TrustedMetadataReconciliationPreservesHandlesAndInvalidatesOldPreparations) {
  Fixture f;f.publish();const auto before=f.generation;
  const auto image=before->resource({Domain::Archive,Kind::Image,0});
  const auto texture=before->resource({Domain::Archive,Kind::Texture,0});
  const auto sampler=before->resource({Domain::Archive,Kind::Sampler,0});
  const std::array selected{TextureResourceSelection{Domain::Archive,Kind::Texture,0}};
  auto history=RetainTextureResourceReferences(before,selected,Pins::History);
  auto oldPreparation=sealedPrepare(before,f.view());auto oldReferences=oldPreparation.selectAll(Pins::Preparation);
  f.archive.images[0].name="Current image label";
  f.archive.images[0].extras=tinygltf::Value(tinygltf::Value::Object{{"current",tinygltf::Value(true)}});
  f.archive.images[0].extensions["VENDOR_image_metadata"]=tinygltf::Value(4);
  f.archive.samplers[0].extras_json_string="{\"label\":2}";
  f.archive.samplers[0].extensions["VENDOR_sampler_metadata"]=tinygltf::Value(5);
  f.archive.textures[0].extensions_json_string="{\"cache\":3}";
  EXPECT_THROW(sealedPrepare(before,f.view()),std::invalid_argument);
  auto reconciliation=sealedReconcile(before,f.view());EXPECT_FALSE(reconciliation.command());
  auto future=reconciliation.select(selected,Pins::History);EXPECT_TRUE(future->valid());EXPECT_FALSE(future->complete());
  bool published{},oldReady{},historyMatches{};size_t attempts{};
  {
    material_publication_test::AllocationGuard guard(true);
    published=ExchangeTextureResourceLifecycle(reconciliation,f.generation,f.view());
    oldReady=CanExchangeTextureResourceLifecycle(oldPreparation,f.generation,f.view());
    historyMatches=history->resourceDependenciesMatch(f.generation);attempts=guard.allocations();
  }
  EXPECT_TRUE(published);EXPECT_FALSE(oldReady);EXPECT_TRUE(historyMatches);EXPECT_EQ(attempts,0u);
  EXPECT_NE(f.generation,before);EXPECT_FALSE(before->matches(f.view()));EXPECT_TRUE(f.generation->matches(f.view()));
  EXPECT_EQ(f.generation->resource({Domain::Archive,Kind::Image,0}),image);
  EXPECT_EQ(f.generation->resource({Domain::Archive,Kind::Texture,0}),texture);
  EXPECT_EQ(f.generation->resource({Domain::Archive,Kind::Sampler,0}),sampler);
  EXPECT_TRUE(history->active());EXPECT_TRUE(oldReferences->valid());
}

TEST(TextureResourceLifecycle, TrustedSourceSuffixRemovalRetiresOriginalHandlesAndBranchAppendUsesNewInternalSlot) {
  Fixture f;f.publish();auto extra=f.evaluated.images[0];extra.name="Evaluated-only image";f.evaluated.images.push_back(extra);
  auto append=sealedReconcile(f.generation,f.view());ASSERT_TRUE(ExchangeTextureResourceLifecycle(append,f.generation,f.view()));
  const auto removed=f.generation->resource({Domain::Evaluated,Kind::Image,1});
  auto oldPreparation=sealedPrepare(f.generation,f.view());auto oldPins=oldPreparation.selectAll(Pins::Preparation);
  f.evaluated.images.resize(1);EXPECT_THROW(sealedPrepare(f.generation,f.view()),std::invalid_argument);
  auto shrink=sealedReconcile(f.generation,f.view());EXPECT_FALSE(shrink.command());
  bool published{};size_t attempts{};
  {material_publication_test::AllocationGuard guard(true);published=ExchangeTextureResourceLifecycle(shrink,f.generation,f.view());attempts=guard.allocations();}
  EXPECT_TRUE(published);EXPECT_EQ(attempts,0u);EXPECT_EQ(f.generation->rowCount(Domain::Evaluated,Kind::Image),1u);
  auto inventory=f.generation->inventory();ASSERT_NE(find(inventory,removed),nullptr);EXPECT_TRUE(find(inventory,removed)->retired);
  EXPECT_TRUE(oldPins->valid());EXPECT_FALSE(oldPins->resourceDependenciesActive());
  f.evaluated.images.push_back(extra);auto branch=sealedReconcile(f.generation,f.view());
  const auto fresh=branch.next()->resource({Domain::Evaluated,Kind::Image,1});EXPECT_NE(fresh,removed);EXPECT_GT(fresh.slot(),removed.slot());
  ASSERT_TRUE(ExchangeTextureResourceLifecycle(branch,f.generation,f.view()));EXPECT_TRUE(f.generation->matches(f.view()));
}

TEST(TextureResourceLifecycle, ReconciliationRejectsTypedBindingsBinaryPayloadAndRuntimeCapabilityReplacement) {
  Fixture f;f.publish();const auto before=f.generation;
  f.archive.images[0].image[0]=9;EXPECT_THROW(sealedReconcile(before,f.view()),std::invalid_argument);f.archive.images[0].image[0]=1;
  f.archive.images[0].uri="another.png";EXPECT_THROW(sealedReconcile(before,f.view()),std::invalid_argument);f.archive.images[0].uri="base.png";
  f.archive.images[0].bits=16;EXPECT_THROW(sealedReconcile(before,f.view()),std::invalid_argument);f.archive.images[0].bits=-1;
  f.archive.samplers[0].wrapS=TINYGLTF_TEXTURE_WRAP_CLAMP_TO_EDGE;
  EXPECT_THROW(sealedReconcile(before,f.view()),std::invalid_argument);f.archive.samplers[0].wrapS=TINYGLTF_TEXTURE_WRAP_REPEAT;
  f.archive.textures[0].source=-1;EXPECT_THROW(sealedReconcile(before,f.view()),std::invalid_argument);f.archive.textures[0].source=0;
  const auto runtime=f.runtime;auto row=*f.runtime.getTexture(0);row.width=7;TextureManager replacement;replacement.registerTexture(row);f.runtime=std::move(replacement);
  EXPECT_THROW(sealedReconcile(before,f.view()),std::invalid_argument);f.runtime=runtime;
  EXPECT_TRUE(before->matches(f.view()));EXPECT_EQ(f.generation,before);
  auto extra=f.archive.images[0];f.archive.images.push_back(extra);auto append=sealedReconcile(before,f.view());
  ASSERT_TRUE(ExchangeTextureResourceLifecycle(append,f.generation,f.view()));
  f.archive.textures[0].extensions["KHR_texture_basisu"]=tinygltf::Value(tinygltf::Value::Object{{"source",tinygltf::Value(1)}});
  EXPECT_THROW(sealedReconcile(f.generation,f.view()),std::invalid_argument);
}

TEST(TextureResourceLifecycle, ImageBufferViewMetadataReconcilesButItsBindingAndBytesRemainExact) {
  Fixture f;f.archive.buffers.emplace_back();f.archive.buffers[0].data={9,1,2,3,4,9};
  tinygltf::BufferView view;view.buffer=0;view.byteOffset=1;view.byteLength=4;f.archive.bufferViews.push_back(view);
  f.archive.images[0].bufferView=0;f.evaluated=f.archive;f.publish();const auto image=f.generation->resource({Domain::Archive,Kind::Image,0});
  f.archive.bufferViews[0].name="Current slice metadata";
  f.archive.bufferViews[0].extensions["VENDOR_slice_metadata"]=tinygltf::Value(4);
  f.archive.bufferViews[0].extras_json_string="{\"tag\":true}";
  auto update=sealedReconcile(f.generation,f.view());ASSERT_TRUE(ExchangeTextureResourceLifecycle(update,f.generation,f.view()));
  EXPECT_EQ(f.generation->resource({Domain::Archive,Kind::Image,0}),image);
  const std::array selection{TextureResourceSelection{Domain::Archive,Kind::Image,0}};
  auto owner=RetainTextureResourceReferences(f.generation,selection,Pins::History);EXPECT_FALSE(owner->complete());
  f.archive.buffers[0].data[1]=8;EXPECT_THROW(sealedReconcile(f.generation,f.view()),std::invalid_argument);f.archive.buffers[0].data[1]=1;
  f.archive.bufferViews[0].byteOffset=0;EXPECT_THROW(sealedReconcile(f.generation,f.view()),std::invalid_argument);
}

TEST(TextureResourceLifecycle, ResourceReplayPreservesTodayPrefixMetadataAcrossTailDetachAndRestore) {
  Fixture f;f.publish();f.append();auto append=sealedPrepare(f.generation,f.view());auto command=append.command();
  ASSERT_TRUE(ExchangeTextureResourceLifecycle(append,f.generation,f.view()));
  const auto savedArchive=f.archive,savedEvaluated=f.evaluated;const auto savedRuntime=f.runtime;
  const auto prefix=f.generation->resource({Domain::Archive,Kind::Image,0});
  f.archive.images[0].extras=tinygltf::Value(tinygltf::Value::Object{{"today",tinygltf::Value(7)}});
  f.archive.images[0].extras_json_string="{\"today\":7}";
  auto metadata=sealedReconcile(f.generation,f.view());ASSERT_TRUE(ExchangeTextureResourceLifecycle(metadata,f.generation,f.view()));
  f.archive.images.resize(1);f.archive.textures.resize(1);f.archive.samplers.resize(1);
  f.evaluated.images.resize(1);f.evaluated.textures.resize(1);f.evaluated.samplers.resize(1);f.runtime=f.runtime.copyPrefix(1);
  const auto today=f.archive.images[0];auto undo=sealedReplay(f.generation,f.view(),command,false);
  ASSERT_TRUE(ExchangeTextureResourceLifecycle(undo,f.generation,f.view()));EXPECT_TRUE(f.generation->matches(f.view()));
  f.archive=savedArchive;f.evaluated=savedEvaluated;f.runtime=savedRuntime;f.archive.images[0]=today;
  auto redo=sealedReplay(f.generation,f.view(),command,true);ASSERT_TRUE(ExchangeTextureResourceLifecycle(redo,f.generation,f.view()));
  EXPECT_TRUE(f.generation->matches(f.view()));EXPECT_EQ(f.generation->resource({Domain::Archive,Kind::Image,0}),prefix);
  EXPECT_EQ(f.archive.images[0].extras_json_string,"{\"today\":7}");
}

TEST(TextureResourceLifecycle, StructuralCandidatesCannotPublishBeforeSourceAndRoleEdgesAreSealed) {
  Fixture f;Roles roles;roles.sync(f);auto prepared=PrepareTextureResourceLifecycle({},roles.view(f));
  EXPECT_TRUE(CanPrepareTextureResourceLifecycle(prepared,f.generation,roles.view(f)));
  EXPECT_FALSE(prepared.sealed());EXPECT_FALSE(CanExchangeTextureResourceLifecycle(prepared,f.generation,roles.view(f)));
  prepared.seal();bool published{};size_t attempts{};
  {material_publication_test::AllocationGuard guard(true);published=ExchangeTextureResourceLifecycle(prepared,f.generation,roles.view(f));attempts=guard.allocations();}
  ASSERT_TRUE(published);EXPECT_EQ(attempts,0u);EXPECT_TRUE(f.generation->liveDependenciesValid());
  const auto snapshot=f.generation->inventory();const auto image=f.generation->resource({Domain::Archive,Kind::Image,0});
  const auto texture=f.generation->resource({Domain::Archive,Kind::Texture,0});const auto runtime=f.generation->resource({Domain::Runtime,Kind::RuntimeTexture,0});
  ASSERT_NE(find(snapshot,image),nullptr);EXPECT_EQ(find(snapshot,image)->pins[size_t(Pins::LiveDependency)],1u);
  EXPECT_EQ(find(snapshot,texture)->pins[size_t(Pins::LiveDependency)],0u); // No source owner self-edge.
  EXPECT_EQ(find(snapshot,runtime)->pins[size_t(Pins::LiveDependency)],2u); // Independent archive/evaluated Color owners.
  EXPECT_EQ(f.generation->sourceLiveOwners().size(),4u);EXPECT_FALSE(f.generation->dependencyAuthorityComplete());
  auto bad=roles;bad.aData.clear();EXPECT_THROW((void)PrepareTextureResourceLifecycle(f.generation,bad.view(f)),std::invalid_argument);
}

TEST(TextureResourceLifecycle, SamplerAliasesReleaseOnlyTheirOwnSourceAndRolePinsAndRestoreOriginalCookies) {
  Fixture f;Roles roles;roles.sync(f);auto initial=sealedPrepare({},roles.view(f));ASSERT_TRUE(ExchangeTextureResourceLifecycle(initial,f.generation,roles.view(f)));
  f.archive.textures.push_back(f.archive.textures.front());f.evaluated=f.archive;roles.sync(f);
  auto appended=sealedPrepare(f.generation,roles.view(f));const auto command=appended.command();const auto aliasCookie=roles.cookies[1];
  ASSERT_TRUE(ExchangeTextureResourceLifecycle(appended,f.generation,roles.view(f)));
  const auto image=f.generation->resource({Domain::Archive,Kind::Image,0});const auto runtime=f.generation->resource({Domain::Runtime,Kind::RuntimeTexture,0});
  auto snapshot=f.generation->inventory();EXPECT_EQ(find(snapshot,image)->pins[size_t(Pins::LiveDependency)],2u);
  EXPECT_EQ(find(snapshot,runtime)->pins[size_t(Pins::LiveDependency)],4u);
  f.archive.textures.resize(1);f.evaluated=f.archive;roles.sync(f);
  auto undo=sealedReplay(f.generation,roles.view(f),command,false);ASSERT_TRUE(ExchangeTextureResourceLifecycle(undo,f.generation,roles.view(f)));
  snapshot=f.generation->inventory();EXPECT_EQ(find(snapshot,image)->pins[size_t(Pins::LiveDependency)],1u);
  EXPECT_EQ(find(snapshot,runtime)->pins[size_t(Pins::LiveDependency)],2u);
  f.archive.textures.push_back(f.archive.textures.front());f.evaluated=f.archive;roles.sync(f);
  EXPECT_THROW((void)PrepareTextureResourceLifecycleReplay(f.generation,roles.view(f),command,true),std::invalid_argument);
  roles.cookies[1]=aliasCookie;auto redo=sealedReplay(f.generation,roles.view(f),command,true);
  ASSERT_TRUE(ExchangeTextureResourceLifecycle(redo,f.generation,roles.view(f)));
  snapshot=f.generation->inventory();EXPECT_EQ(find(snapshot,runtime)->pins[size_t(Pins::LiveDependency)],4u);
}

TEST(TextureResourceLifecycle, OneRoleChangeReusesUnrelatedArchiveAndEvaluatedOwnerControls) {
  Fixture f;f.append();Roles roles;roles.sync(f);auto initial=sealedPrepare({},roles.view(f));
  ASSERT_TRUE(ExchangeTextureResourceLifecycle(initial,f.generation,roles.view(f)));
  const auto archive0=f.generation->resource({Domain::Archive,Kind::Texture,0});
  const auto archive1=f.generation->resource({Domain::Archive,Kind::Texture,1});
  const auto evaluated0=f.generation->resource({Domain::Evaluated,Kind::Texture,0});
  const auto role=[&](const TextureResourceLifecycleGenerationPtr& generation,const ResourceHandle& owner) {
    for(const auto& candidate:generation->sourceLiveOwners())
      if(candidate->key().owner==owner && candidate->key().kind==TextureResourceLiveOwnerKind::ColorRole) return candidate;
    return TextureResourceLiveOwnerPtr{};
  };
  const auto changed=role(f.generation,archive0),unchangedArchive=role(f.generation,archive1),unchangedEvaluated=role(f.generation,evaluated0);
  ASSERT_TRUE(changed);ASSERT_TRUE(unchangedArchive);ASSERT_TRUE(unchangedEvaluated);
  roles.aColor[0]=1;auto update=PrepareTextureResourceLifecycle(f.generation,roles.view(f));
  EXPECT_NE(role(update.next(),archive0),changed);EXPECT_EQ(role(update.next(),archive1),unchangedArchive);
  EXPECT_EQ(role(update.next(),evaluated0),unchangedEvaluated);update.seal();
  ASSERT_TRUE(ExchangeTextureResourceLifecycle(update,f.generation,roles.view(f)));
  EXPECT_FALSE(changed->active());EXPECT_TRUE(unchangedArchive->active());EXPECT_TRUE(unchangedEvaluated->active());
  const auto zero=f.generation->resource({Domain::Runtime,Kind::RuntimeTexture,0});
  const auto one=f.generation->resource({Domain::Runtime,Kind::RuntimeTexture,1});const auto snapshot=f.generation->inventory();
  EXPECT_EQ(find(snapshot,zero)->pins[size_t(Pins::LiveDependency)],3u);EXPECT_EQ(find(snapshot,one)->pins[size_t(Pins::LiveDependency)],1u);
}

TEST(TextureResourceLifecycle, BoundTailUndoWaitsForExactPairedMaterialReleaseAndRedoUsesOneActivation) {
  Fixture f;f.publish();f.append();auto append=sealedPrepare(f.generation,f.view());auto command=append.command();
  ASSERT_TRUE(ExchangeTextureResourceLifecycle(append,f.generation,f.view()));
  ResourceLifecycleCatalog materials;const auto domain=materials.createDomain(Domain::Runtime);const auto material=materials.issue(domain,Kind::Material,0);
  const std::array tail{TextureResourceSelection{Domain::Runtime,Kind::RuntimeTexture,1}};
  auto bind=PrepareTextureResourceLifecycle(f.generation,f.view());auto live=bind.stageLiveOwner({material,TextureResourceLiveOwnerKind::Material,0},tail);
  EXPECT_TRUE(live->pending());EXPECT_TRUE(live->matches(bind.next()));bind.seal();
  ASSERT_TRUE(ExchangeTextureResourceLifecycle(bind,f.generation,f.view()));ASSERT_TRUE(live->active());
  const auto original=f.generation->resource(tail[0]);f.trim();
  {
    auto resourceOnly=PrepareTextureResourceLifecycleReplay(f.generation,f.view(),command,false);
    EXPECT_TRUE(CanPrepareTextureResourceLifecycle(resourceOnly,f.generation,f.view()));
    auto prefix=resourceOnly.selectAll(Pins::Preparation);EXPECT_TRUE(prefix->active());
    EXPECT_FALSE(CanExchangeTextureResourceLifecycle(resourceOnly,f.generation,f.view()));
    EXPECT_THROW(resourceOnly.seal(),std::invalid_argument);EXPECT_TRUE(live->active());
  }
  auto combined=PrepareTextureResourceLifecycleReplay(f.generation,f.view(),command,false);
  auto cleared=combined.stageLiveOwner({material,TextureResourceLiveOwnerKind::Material,0},{},live);combined.seal();
  const auto before=f.generation->revision();bool published{};size_t attempts{};
  {material_publication_test::AllocationGuard guard(true);published=ExchangeTextureResourceLifecycle(combined,f.generation,f.view());attempts=guard.allocations();}
  ASSERT_TRUE(published);EXPECT_EQ(attempts,0u);EXPECT_EQ(f.generation->revision(),before+1);EXPECT_FALSE(live->active());EXPECT_TRUE(cleared->active());
  EXPECT_GT(cleared->key().version,live->key().version);
  f.append();auto restore=PrepareTextureResourceLifecycleReplay(f.generation,f.view(),command,true);
  auto restored=restore.stageLiveOwner({material,TextureResourceLiveOwnerKind::Material,0},tail,cleared);restore.seal();
  const auto beforeRestore=f.generation->revision();
  {material_publication_test::AllocationGuard guard(true);published=ExchangeTextureResourceLifecycle(restore,f.generation,f.view());attempts=guard.allocations();}
  ASSERT_TRUE(published);EXPECT_EQ(attempts,0u);EXPECT_EQ(f.generation->revision(),beforeRestore+1);
  EXPECT_EQ(f.generation->resource(tail[0]),original);EXPECT_TRUE(restored->active());EXPECT_FALSE(cleared->active());
}

TEST(TextureResourceLifecycle, NewResourceAndMaterialEdgesPublishInOneRegistrationAndAbandonmentKeepsOldOwners) {
  Fixture f;f.publish();ResourceLifecycleCatalog materials;auto domain=materials.createDomain(Domain::Runtime);auto material=materials.issue(domain,Kind::Material,0);
  auto begin=PrepareTextureResourceLifecycle(f.generation,f.view());auto empty=begin.stageLiveOwner({material,TextureResourceLiveOwnerKind::Material,0},{});
  begin.seal();ASSERT_TRUE(ExchangeTextureResourceLifecycle(begin,f.generation,f.view()));f.append();
  const std::array target{TextureResourceSelection{Domain::Runtime,Kind::RuntimeTexture,1}};TextureResourceLiveOwnerPtr abandoned;
  {
    auto candidate=PrepareTextureResourceLifecycle(f.generation,f.view());abandoned=candidate.stageLiveOwner({material,TextureResourceLiveOwnerKind::Material,0},target,empty);
    candidate.seal();EXPECT_TRUE(abandoned->pending());EXPECT_TRUE(empty->active());
  }
  EXPECT_FALSE(abandoned->valid());EXPECT_TRUE(empty->active());
  auto candidate=PrepareTextureResourceLifecycle(f.generation,f.view());auto future=candidate.stageLiveOwner({material,TextureResourceLiveOwnerKind::Material,0},target,empty);
  candidate.seal();const auto revision=f.generation->revision();bool published{};size_t attempts{};
  {material_publication_test::AllocationGuard guard(true);published=ExchangeTextureResourceLifecycle(candidate,f.generation,f.view());attempts=guard.allocations();}
  ASSERT_TRUE(published);EXPECT_EQ(attempts,0u);EXPECT_EQ(f.generation->revision(),revision+1);EXPECT_TRUE(future->active());EXPECT_FALSE(empty->active());
}

TEST(TextureResourceLifecycle, ResidualExternalLiveOwnerBlocksAfterAllOwnedRoleEdgesAreReleased) {
  Fixture f;Roles roles;roles.sync(f);auto initial=sealedPrepare({},roles.view(f));ASSERT_TRUE(ExchangeTextureResourceLifecycle(initial,f.generation,roles.view(f)));
  const std::array selected{TextureResourceSelection{Domain::Runtime,Kind::RuntimeTexture,0}};
  auto external=RetainTextureResourceReferences(f.generation,selected,Pins::LiveDependency);
  ASSERT_TRUE(f.runtime.retireTexture(0));roles.aColor[0]=roles.eColor[0]=UINT32_MAX;
  {
    auto candidate=PrepareTextureResourceLifecycle(f.generation,roles.view(f));EXPECT_TRUE(CanPrepareTextureResourceLifecycle(candidate,f.generation,roles.view(f)));
    EXPECT_THROW(candidate.seal(),std::invalid_argument);EXPECT_TRUE(external->active());
  }
  external.reset();auto candidate=sealedPrepare(f.generation,roles.view(f));ASSERT_TRUE(ExchangeTextureResourceLifecycle(candidate,f.generation,roles.view(f)));
  EXPECT_TRUE(f.generation->liveDependenciesValid());EXPECT_TRUE(f.generation->matches(roles.view(f)));
}

TEST(TextureResourceLifecycle, MetadataReplacementDeactivatesOldControlsAndEquivalentSnapshotsNeedRestaging) {
  Fixture f;f.publish();const auto before=f.generation;const auto old=before->sourceLiveOwners().front();
  const auto image=before->resource({Domain::Archive,Kind::Image,0});f.archive.textures[0].name="Current metadata";
  auto update=ReconcileTextureResourceLifecycle(before,f.view());EXPECT_TRUE(old->active());
  update.seal();ASSERT_TRUE(ExchangeTextureResourceLifecycle(update,f.generation,f.view()));
  EXPECT_FALSE(old->active());EXPECT_TRUE(before->valid());EXPECT_FALSE(before->liveDependenciesValid());EXPECT_FALSE(before->matches(f.view()));
  auto snapshot=f.generation->inventory();EXPECT_EQ(find(snapshot,image)->pins[size_t(Pins::LiveDependency)],1u);
  // Restage the historical source view against the ACTUAL current generation.
  f.archive.textures[0].name="Base texture";auto restore=ReconcileTextureResourceLifecycle(f.generation,f.view());restore.seal();
  ASSERT_TRUE(ExchangeTextureResourceLifecycle(restore,f.generation,f.view()));EXPECT_TRUE(f.generation->matches(f.view()));
  EXPECT_EQ(f.generation->resource({Domain::Archive,Kind::Image,0}),image);
}

TEST(TextureResourceLifecycle, EmptyLiveOwnersStillProveSourceAuthorityAndRejectStaleReplacement) {
  Fixture f;f.publish();Fixture foreign;foreign.publish();ResourceLifecycleCatalog materials;auto domain=materials.createDomain(Domain::Runtime);auto material=materials.issue(domain,Kind::Material,0);
  auto prepared=PrepareTextureResourceLifecycle(f.generation,f.view());auto owner=prepared.stageLiveOwner({material,TextureResourceLiveOwnerKind::Material,0},{}, {},64ull*1024*1024,true);
  EXPECT_TRUE(owner->matches(prepared.next()));EXPECT_FALSE(owner->matches(foreign.generation));EXPECT_FALSE(owner->complete());
  prepared.seal();ASSERT_TRUE(ExchangeTextureResourceLifecycle(prepared,f.generation,f.view()));
  auto missingOld=PrepareTextureResourceLifecycle(f.generation,f.view());
  EXPECT_THROW((void)missingOld.stageLiveOwner({material,TextureResourceLiveOwnerKind::Material,0},{}),std::invalid_argument);
  auto next=PrepareTextureResourceLifecycle(f.generation,f.view());auto replacement=next.stageLiveOwner({material,TextureResourceLiveOwnerKind::Material,0},{},owner);next.seal();
  ASSERT_TRUE(ExchangeTextureResourceLifecycle(next,f.generation,f.view()));EXPECT_FALSE(owner->valid());EXPECT_TRUE(replacement->active());
  auto stale=PrepareTextureResourceLifecycle(f.generation,f.view());EXPECT_THROW((void)stale.stageLiveOwner({material,TextureResourceLiveOwnerKind::Material,0},{},owner),std::invalid_argument);
}

TEST(TextureResourceLifecycle, FiveThousandIndependentTexturedOwnersUseDeclaredFiniteQuotasWithoutLegacyPinCeiling) {
  Fixture f;f.publish();ResourceLifecycleCatalog materials;const auto domain=materials.createDomain(Domain::Runtime);
  std::vector<ResourceRegistrationRequest> requests;requests.reserve(5000);
  for(uint32_t i=0;i<5000;++i) requests.push_back({domain,Kind::Material,i});
  auto registration=materials.prepareIssue(requests);const std::vector<ResourceHandle> handles(registration.resources().begin(),registration.resources().end());
  ASSERT_TRUE(materials.exchange(registration));auto candidate=PrepareTextureResourceLifecycle(f.generation,f.view());
  const std::array selection{TextureResourceSelection{Domain::Runtime,Kind::RuntimeTexture,0}};
  std::vector<TextureResourceLiveOwnerPtr> owners;owners.reserve(handles.size());
  for(const auto& handle:handles) owners.push_back(candidate.stageLiveOwner({handle,TextureResourceLiveOwnerKind::Material,0},selection));
  candidate.seal();bool published{};size_t attempts{};
  {material_publication_test::AllocationGuard guard(true);published=ExchangeTextureResourceLifecycle(candidate,f.generation,f.view());attempts=guard.allocations();}
  ASSERT_TRUE(published);EXPECT_EQ(attempts,0u);for(const auto& owner:owners) EXPECT_TRUE(owner->active());
  const auto runtime=f.generation->resource(selection[0]);const auto inventory=f.generation->inventory();
  ASSERT_NE(find(inventory,runtime),nullptr);EXPECT_EQ(find(inventory,runtime)->pins[size_t(Pins::LiveDependency)],5000u);
  TextureResourceLifecycleLimits limits;limits.maxLiveOwners=5002; // Two automatic source texture controls already exist.
  auto bounded=PrepareTextureResourceLifecycle(f.generation,f.view(),limits);
  EXPECT_THROW((void)bounded.stageLiveOwner({handles[0],TextureResourceLiveOwnerKind::Rest,0},selection),std::length_error);
  EXPECT_TRUE(owners.front()->active());
}

TEST(TextureResourceLifecycle, FutureOwnerGuardRejectsOutgoingControlsAndAnotherCandidatesSameKeyPendingOwner) {
  Fixture f;f.publish();ResourceLifecycleCatalog materials;const auto domain=materials.createDomain(Domain::Runtime);const auto material=materials.issue(domain,Kind::Material,0);
  const std::array targets{TextureResourceSelection{Domain::Runtime,Kind::RuntimeTexture,0}};
  auto initial=PrepareTextureResourceLifecycle(f.generation,f.view());auto live=initial.stageLiveOwner({material,TextureResourceLiveOwnerKind::Material,0},targets);
  initial.seal();ASSERT_TRUE(ExchangeTextureResourceLifecycle(initial,f.generation,f.view()));
  {
    auto release=PrepareTextureResourceLifecycle(f.generation,f.view());const std::array outgoing{live};release.releaseLiveOwners(outgoing);
    bool retained{};size_t attempts{};
    {material_publication_test::AllocationGuard deny(true);retained=release.liveOwnerWillBeActive(live);attempts=deny.allocations();}
    EXPECT_FALSE(retained);EXPECT_EQ(attempts,0u);EXPECT_TRUE(live->active());
  }
  auto first=PrepareTextureResourceLifecycle(f.generation,f.view());auto second=PrepareTextureResourceLifecycle(f.generation,f.view());
  auto future=first.stageLiveOwner({material,TextureResourceLiveOwnerKind::Material,0},targets,live);
  auto other=second.stageLiveOwner({material,TextureResourceLiveOwnerKind::Material,0},targets,live);
  EXPECT_EQ(future->key(),other->key());EXPECT_NE(future,other);bool own{},foreign{},old{};size_t attempts{};
  {material_publication_test::AllocationGuard deny(true);own=first.liveOwnerWillBeActive(future);foreign=first.liveOwnerWillBeActive(other);old=first.liveOwnerWillBeActive(live);attempts=deny.allocations();}
  EXPECT_TRUE(own);EXPECT_FALSE(foreign);EXPECT_FALSE(old);EXPECT_EQ(attempts,0u);
  first={};EXPECT_FALSE(future->valid());EXPECT_FALSE(second.liveOwnerWillBeActive(future));EXPECT_TRUE(live->active());
  EXPECT_TRUE(second.liveOwnerWillBeActive(other));second.seal();ASSERT_TRUE(ExchangeTextureResourceLifecycle(second,f.generation,f.view()));
  EXPECT_TRUE(other->active());EXPECT_FALSE(live->active());
}

TEST(TextureResourceLifecycle, SourceMasksBootstrapStableInactiveRowsAndRejectInvalidMasks) {
  Fixture f;f.append();SourcePhases phases;phases.sync(f);phases.aImages[1]=phases.aTextures[1]=phases.aSamplers[1]=0;
  auto view=phases.apply(f.view());auto prepared=PrepareTextureResourceLifecycle({},view);
  EXPECT_TRUE(prepared.next()->active({Domain::Archive,Kind::Texture,0}));
  EXPECT_FALSE(prepared.next()->active({Domain::Archive,Kind::Texture,1}));
  const auto retired=prepared.next()->resource({Domain::Archive,Kind::Texture,1});ASSERT_TRUE(retired);prepared.seal();
  bool published{};size_t attempts{};
  {material_publication_test::AllocationGuard deny(true);published=ExchangeTextureResourceLifecycle(prepared,f.generation,view);attempts=deny.allocations();}
  ASSERT_TRUE(published);EXPECT_EQ(attempts,0u);EXPECT_TRUE(f.generation->matches(view));EXPECT_FALSE(f.generation->matches(f.view()));
  EXPECT_EQ(f.generation->rowCount(Domain::Archive,Kind::Texture),2u);EXPECT_FALSE(f.generation->active({Domain::Archive,Kind::Texture,1}));
  EXPECT_TRUE(f.generation->active({Domain::Evaluated,Kind::Texture,1}));
  const auto inventory=f.generation->inventory();ASSERT_NE(find(inventory,retired),nullptr);EXPECT_TRUE(find(inventory,retired)->retired);
  phases.aTextures[1]=2;EXPECT_FALSE(f.generation->matches(phases.apply(f.view())));
  EXPECT_THROW((void)PrepareTextureResourceLifecycle({},phases.apply(f.view())),std::invalid_argument);
  phases.aTextures={1};EXPECT_THROW((void)PrepareTextureResourceLifecycle({},phases.apply(f.view())),std::invalid_argument);
  EXPECT_FALSE(f.generation->dependencyAuthorityComplete());
}

TEST(TextureResourceLifecycle, ActiveTexturesRejectInactiveCoreAlternativeAndSamplerTargets) {
  Fixture f;f.append();SourcePhases phases;phases.sync(f);phases.aImages[0]=0;
  EXPECT_THROW((void)PrepareTextureResourceLifecycle({},phases.apply(f.view())),std::invalid_argument);
  phases.aImages[0]=1;phases.aSamplers[0]=0;
  EXPECT_THROW((void)PrepareTextureResourceLifecycle({},phases.apply(f.view())),std::invalid_argument);
  phases.aSamplers[0]=1;phases.aImages[1]=0;phases.aTextures[1]=0;
  f.archive.textures[0].extensions["KHR_texture_basisu"]=tinygltf::Value(tinygltf::Value::Object{{"source",tinygltf::Value(1)}});
  EXPECT_THROW((void)PrepareTextureResourceLifecycle({},phases.apply(f.view())),std::invalid_argument);
}

TEST(TextureResourceLifecycle, SelectedMiddleTextureRetirementNeedsSealAndOriginalReceiptForRestore) {
  Fixture f;f.append();f.append("Third");f.publish();SourcePhases phases;phases.sync(f);phases.aTextures[1]=0;
  const auto view=phases.apply(f.view());const auto handle=f.generation->resource({Domain::Archive,Kind::Texture,1});
  const std::array selected{TextureResourceSelection{Domain::Archive,Kind::Texture,1}};
  EXPECT_THROW((void)PrepareTextureResourceLifecycle(f.generation,view),std::invalid_argument);
  EXPECT_THROW((void)ReconcileTextureResourceLifecycle(f.generation,view),std::invalid_argument);
  auto retirement=PrepareTextureResourceRetirement(f.generation,view,selected);const auto command=retirement.retirementCommand();
  ASSERT_TRUE(command);EXPECT_EQ(command.selections().size(),1u);ASSERT_TRUE(command.references());
  EXPECT_TRUE(CanPrepareTextureResourceLifecycle(retirement,f.generation,view));EXPECT_FALSE(CanExchangeTextureResourceLifecycle(retirement,f.generation,view));
  retirement.seal();bool published{};size_t attempts{};
  {material_publication_test::AllocationGuard deny(true);published=ExchangeTextureResourceLifecycle(retirement,f.generation,view);attempts=deny.allocations();}
  ASSERT_TRUE(published);EXPECT_EQ(attempts,0u);EXPECT_EQ(f.generation->rowCount(Domain::Archive,Kind::Texture),3u);
  EXPECT_EQ(f.generation->resource(selected[0]),handle);EXPECT_FALSE(f.generation->active(selected[0]));
  EXPECT_TRUE(command.references()->valid());EXPECT_FALSE(command.references()->resourceDependenciesActive());
  phases.aTextures[1]=1;const auto restoredView=phases.apply(f.view());
  EXPECT_THROW((void)PrepareTextureResourceLifecycle(f.generation,restoredView),std::invalid_argument);
  auto restore=PrepareTextureResourceRetirementReplay(f.generation,restoredView,command,ResourceLifecyclePhase::Active);restore.seal();
  ASSERT_TRUE(ExchangeTextureResourceLifecycle(restore,f.generation,restoredView));EXPECT_TRUE(f.generation->active(selected[0]));
  EXPECT_EQ(f.generation->resource(selected[0]),handle);
  EXPECT_THROW((void)PrepareTextureResourceRetirementReplay(f.generation,restoredView,command,ResourceLifecyclePhase::Active),std::invalid_argument);
}

TEST(TextureResourceLifecycle, SelectedTextureImageSamplerBundleRoundTripsExactHandlesWithoutAllocating) {
  Fixture f;f.publish();SourcePhases phases;phases.sync(f);
  const std::array selected{TextureResourceSelection{Domain::Archive,Kind::Texture,0},
      TextureResourceSelection{Domain::Archive,Kind::Image,0},TextureResourceSelection{Domain::Archive,Kind::Sampler,0}};
  std::array<ResourceHandle,3> handles;for(size_t i=0;i<selected.size();++i) handles[i]=f.generation->resource(selected[i]);
  phases.aImages[0]=phases.aTextures[0]=phases.aSamplers[0]=0;std::optional<TextureResourceLifecycleView> view;view.emplace(phases.apply(f.view()));
  auto retire=PrepareTextureResourceRetirement(f.generation,*view,selected);const auto command=retire.retirementCommand();retire.seal();
  bool retired{},restored{};size_t attempts{};
  {material_publication_test::AllocationGuard deny(true);retired=ExchangeTextureResourceLifecycle(retire,f.generation,*view);attempts=deny.allocations();}
  ASSERT_TRUE(retired);EXPECT_EQ(attempts,0u);for(size_t i=0;i<selected.size();++i) {EXPECT_FALSE(f.generation->active(selected[i]));EXPECT_EQ(f.generation->resource(selected[i]),handles[i]);}
  phases.aImages[0]=phases.aTextures[0]=phases.aSamplers[0]=1;view.emplace(phases.apply(f.view()));
  auto restore=PrepareTextureResourceRetirementReplay(f.generation,*view,command,ResourceLifecyclePhase::Active);restore.seal();
  {material_publication_test::AllocationGuard deny(true);restored=ExchangeTextureResourceLifecycle(restore,f.generation,*view);attempts=deny.allocations();}
  ASSERT_TRUE(restored);EXPECT_EQ(attempts,0u);for(size_t i=0;i<selected.size();++i) {EXPECT_TRUE(f.generation->active(selected[i]));EXPECT_EQ(f.generation->resource(selected[i]),handles[i]);}
}

TEST(TextureResourceLifecycle, SurvivingAlternativeOwnerBlocksSelectedImageAndSamplerRetirement) {
  Fixture f;f.append();f.archive.textures[0].extensions["EXT_texture_webp"]=tinygltf::Value(tinygltf::Value::Object{{"source",tinygltf::Value(1)}});f.publish();
  SourcePhases phases;phases.sync(f);phases.aImages[1]=0;const std::array image{TextureResourceSelection{Domain::Archive,Kind::Image,1}};
  EXPECT_THROW((void)PrepareTextureResourceRetirement(f.generation,phases.apply(f.view()),image),std::invalid_argument);
  phases.aImages[1]=1;phases.aSamplers[0]=0;const std::array sampler{TextureResourceSelection{Domain::Archive,Kind::Sampler,0}};
  EXPECT_THROW((void)PrepareTextureResourceRetirement(f.generation,phases.apply(f.view()),sampler),std::invalid_argument);
  EXPECT_TRUE(f.generation->active(image[0]));EXPECT_TRUE(f.generation->active(sampler[0]));
}

TEST(TextureResourceLifecycle, SelectedRetirementReleasesOnlyExactAuthorizedLiveOwners) {
  Fixture f;f.publish();ResourceLifecycleCatalog materials;const auto domain=materials.createDomain(Domain::Runtime);const auto material=materials.issue(domain,Kind::Material,0);
  const std::array selected{TextureResourceSelection{Domain::Archive,Kind::Texture,0}};
  auto initial=PrepareTextureResourceLifecycle(f.generation,f.view());
  auto owner=initial.stageLiveOwner({material,TextureResourceLiveOwnerKind::Material,0},selected);
  auto external=initial.stageLiveOwner({material,TextureResourceLiveOwnerKind::Provider,0},selected);
  initial.seal();ASSERT_TRUE(ExchangeTextureResourceLifecycle(initial,f.generation,f.view()));
  SourcePhases phases;phases.sync(f);phases.aTextures[0]=0;const auto view=phases.apply(f.view());const auto revision=f.generation->revision();
  auto blocked=PrepareTextureResourceRetirement(f.generation,view,selected);EXPECT_THROW(blocked.seal(),std::invalid_argument);
  EXPECT_EQ(f.generation->revision(),revision);EXPECT_TRUE(owner->active());EXPECT_TRUE(external->active());
  auto residual=PrepareTextureResourceRetirement(f.generation,view,selected);const std::array one{owner};residual.releaseLiveOwners(one);
  EXPECT_THROW(residual.seal(),std::invalid_argument);EXPECT_TRUE(owner->active());EXPECT_TRUE(external->active());
  auto paired=PrepareTextureResourceRetirement(f.generation,view,selected);const std::array both{owner,external};paired.releaseLiveOwners(both);paired.seal();
  ASSERT_TRUE(ExchangeTextureResourceLifecycle(paired,f.generation,view));EXPECT_FALSE(owner->active());EXPECT_FALSE(external->active());
}

TEST(TextureResourceLifecycle, SelectedRoleReplayPreservesUnrelatedCurrentMetadataRolesAndCookies) {
  Fixture f;f.append();Roles roles;roles.sync(f);SourcePhases phases;phases.sync(f);
  std::optional<TextureResourceLifecycleView> view;view.emplace(phases.apply(roles.view(f)));auto initial=sealedPrepare({},*view);ASSERT_TRUE(ExchangeTextureResourceLifecycle(initial,f.generation,*view));
  const std::array selected{TextureResourceSelection{Domain::Archive,Kind::Texture,0},TextureResourceSelection{Domain::Evaluated,Kind::Texture,0}};
  const auto cookie=roles.cookies[0];phases.aTextures[0]=phases.eTextures[0]=0;roles.aColor[0]=roles.eColor[0]=UINT32_MAX;roles.mapping[0]=-1;
  view.emplace(phases.apply(roles.view(f)));auto retire=PrepareTextureResourceRetirement(f.generation,*view,selected);const auto command=retire.retirementCommand();retire.seal();
  ASSERT_TRUE(ExchangeTextureResourceLifecycle(retire,f.generation,*view));
  f.archive.textures[1].name="Current prefix metadata";f.evaluated.textures[1].name="Current evaluated metadata";
  roles.aColor[1]=roles.eColor[1]=1;roles.cookies[1]=std::make_shared<const uint8_t>(7);const auto unrelatedCookie=roles.cookies[1];
  view.emplace(phases.apply(roles.view(f)));auto maintenance=sealedReconcile(f.generation,*view);ASSERT_TRUE(ExchangeTextureResourceLifecycle(maintenance,f.generation,*view));
  phases.aTextures[0]=phases.eTextures[0]=1;roles.aColor[0]=roles.eColor[0]=0;roles.mapping[0]=0;view.emplace(phases.apply(roles.view(f)));
  auto restore=PrepareTextureResourceRetirementReplay(f.generation,*view,command,ResourceLifecyclePhase::Active);restore.seal();
  bool restored{};size_t attempts{};
  {material_publication_test::AllocationGuard deny(true);restored=ExchangeTextureResourceLifecycle(restore,f.generation,*view);attempts=deny.allocations();}
  ASSERT_TRUE(restored);EXPECT_EQ(attempts,0u);EXPECT_EQ(roles.cookies[0],cookie);EXPECT_EQ(roles.cookies[1],unrelatedCookie);
  EXPECT_EQ(roles.aColor[1],1u);EXPECT_EQ(f.archive.textures[1].name,"Current prefix metadata");EXPECT_TRUE(f.generation->matches(*view));
  phases.aTextures[0]=phases.eTextures[0]=0;roles.aColor[0]=roles.eColor[0]=UINT32_MAX;roles.mapping[0]=-1;view.emplace(phases.apply(roles.view(f)));
  auto redo=PrepareTextureResourceRetirementReplay(f.generation,*view,command,ResourceLifecyclePhase::Retired);redo.seal();
  ASSERT_TRUE(ExchangeTextureResourceLifecycle(redo,f.generation,*view));EXPECT_EQ(roles.cookies[1],unrelatedCookie);
}

TEST(TextureResourceLifecycle, EvaluatedRetirementCannotClearAnUnselectedArchiveCorrespondence) {
  Fixture f;Roles roles;roles.sync(f);SourcePhases phases;phases.sync(f);std::optional<TextureResourceLifecycleView> view;view.emplace(phases.apply(roles.view(f)));
  auto initial=sealedPrepare({},*view);ASSERT_TRUE(ExchangeTextureResourceLifecycle(initial,f.generation,*view));
  phases.eTextures[0]=0;roles.eColor[0]=UINT32_MAX;const std::array evaluated{TextureResourceSelection{Domain::Evaluated,Kind::Texture,0}};
  EXPECT_THROW((void)PrepareTextureResourceRetirement(f.generation,phases.apply(roles.view(f)),evaluated),std::invalid_argument);
  roles.mapping[0]=-1;EXPECT_THROW((void)PrepareTextureResourceRetirement(f.generation,phases.apply(roles.view(f)),evaluated),std::invalid_argument);
  phases.aTextures[0]=0;roles.aColor[0]=UINT32_MAX;
  const std::array pair{TextureResourceSelection{Domain::Archive,Kind::Texture,0},evaluated[0]};view.emplace(phases.apply(roles.view(f)));
  auto retirement=PrepareTextureResourceRetirement(f.generation,*view,pair);retirement.seal();ASSERT_TRUE(ExchangeTextureResourceLifecycle(retirement,f.generation,*view));
}

TEST(TextureResourceLifecycle, SelectedReplayRejectsForeignReceiptsChangedPayloadsAndReusedSourceRows) {
  Fixture f;f.publish();SourcePhases phases;phases.sync(f);phases.aTextures[0]=0;std::optional<TextureResourceLifecycleView> view;view.emplace(phases.apply(f.view()));
  const std::array selected{TextureResourceSelection{Domain::Archive,Kind::Texture,0}};const auto original=f.generation->resource(selected[0]);
  auto retire=PrepareTextureResourceRetirement(f.generation,*view,selected);const auto command=retire.retirementCommand();retire.seal();ASSERT_TRUE(ExchangeTextureResourceLifecycle(retire,f.generation,*view));
  Fixture foreign;foreign.publish();EXPECT_THROW((void)PrepareTextureResourceRetirementReplay(foreign.generation,foreign.view(),command,ResourceLifecyclePhase::Active),std::invalid_argument);
  phases.aTextures[0]=1;f.archive.textures[0].name="Replaced retired metadata";
  EXPECT_THROW((void)PrepareTextureResourceRetirementReplay(f.generation,phases.apply(f.view()),command,ResourceLifecyclePhase::Active),std::invalid_argument);
  f.archive.textures[0].name="Base texture";
  // A trusted suffix trim and a fresh append reuse the numeric row, never its
  // retained retirement incarnation, even when all source fields compare equal.
  const auto replacement=f.archive.textures[0];f.archive.textures.clear();phases.aTextures.clear();view.emplace(phases.apply(f.view()));
  auto trim=sealedReconcile(f.generation,*view);ASSERT_TRUE(ExchangeTextureResourceLifecycle(trim,f.generation,*view));
  f.archive.textures.push_back(replacement);phases.aTextures={1};view.emplace(phases.apply(f.view()));auto append=sealedPrepare(f.generation,*view);
  ASSERT_TRUE(ExchangeTextureResourceLifecycle(append,f.generation,*view));EXPECT_NE(f.generation->resource(selected[0]),original);
  EXPECT_THROW((void)PrepareTextureResourceRetirementReplay(f.generation,*view,command,ResourceLifecyclePhase::Active),std::invalid_argument);
}

TEST(TextureResourceLifecycle, SelectedRetirementValidatesSelectionBudgetsAndExactMaskWitnesses) {
  Fixture f;f.publish();SourcePhases phases;phases.sync(f);phases.aTextures[0]=0;auto view=phases.apply(f.view());
  const std::array selected{TextureResourceSelection{Domain::Archive,Kind::Texture,0}};
  EXPECT_THROW((void)PrepareTextureResourceRetirement(f.generation,view,{}),std::invalid_argument);
  const std::array duplicates{selected[0],selected[0]};EXPECT_THROW((void)PrepareTextureResourceRetirement(f.generation,view,duplicates),std::invalid_argument);
  const std::array runtime{TextureResourceSelection{Domain::Runtime,Kind::RuntimeTexture,0}};EXPECT_THROW((void)PrepareTextureResourceRetirement(f.generation,view,runtime),std::invalid_argument);
  TextureResourceLifecycleLimits limits;limits.maxOwnedBytes=1;EXPECT_THROW((void)PrepareTextureResourceRetirement(f.generation,view,selected,limits),std::length_error);
  auto candidate=PrepareTextureResourceRetirement(f.generation,view,selected);candidate.seal();phases.aTextures[0]=1;
  bool ready{},published{};size_t attempts{};
  {material_publication_test::AllocationGuard deny(true);ready=CanExchangeTextureResourceLifecycle(candidate,f.generation,view);published=ExchangeTextureResourceLifecycle(candidate,f.generation,view);attempts=deny.allocations();}
  EXPECT_FALSE(ready);EXPECT_FALSE(published);EXPECT_EQ(attempts,0u);EXPECT_TRUE(f.generation->active(selected[0]));
}

TEST(TextureResourceLifecycle, SelectedTextureRestoreRequiresItsDependenciesToBeRestoredByTheirOwnReceipts) {
  Fixture f;f.publish();SourcePhases phases;phases.sync(f);const std::array texture{TextureResourceSelection{Domain::Archive,Kind::Texture,0}};
  phases.aTextures[0]=0;std::optional<TextureResourceLifecycleView> view;view.emplace(phases.apply(f.view()));auto removeTexture=PrepareTextureResourceRetirement(f.generation,*view,texture);
  const auto textureCommand=removeTexture.retirementCommand();removeTexture.seal();ASSERT_TRUE(ExchangeTextureResourceLifecycle(removeTexture,f.generation,*view));
  phases.aImages[0]=0;view.emplace(phases.apply(f.view()));const std::array image{TextureResourceSelection{Domain::Archive,Kind::Image,0}};
  auto removeImage=PrepareTextureResourceRetirement(f.generation,*view,image);const auto imageCommand=removeImage.retirementCommand();removeImage.seal();
  ASSERT_TRUE(ExchangeTextureResourceLifecycle(removeImage,f.generation,*view));EXPECT_TRUE(textureCommand.references()->valid());
  phases.aTextures[0]=1;EXPECT_THROW((void)PrepareTextureResourceRetirementReplay(f.generation,phases.apply(f.view()),textureCommand,ResourceLifecyclePhase::Active),std::invalid_argument);
  phases.aTextures[0]=0;phases.aImages[0]=1;view.emplace(phases.apply(f.view()));
  auto restoreImage=PrepareTextureResourceRetirementReplay(f.generation,*view,imageCommand,ResourceLifecyclePhase::Active);restoreImage.seal();
  ASSERT_TRUE(ExchangeTextureResourceLifecycle(restoreImage,f.generation,*view));phases.aTextures[0]=1;view.emplace(phases.apply(f.view()));
  auto restoreTexture=PrepareTextureResourceRetirementReplay(f.generation,*view,textureCommand,ResourceLifecyclePhase::Active);restoreTexture.seal();
  ASSERT_TRUE(ExchangeTextureResourceLifecycle(restoreTexture,f.generation,*view));EXPECT_TRUE(f.generation->active(texture[0]));
}

TEST(TextureResourceLifecycle, SelectedSourceCookieCannotChangeDuringRestoreAndCanceledReceiptCannotActivate) {
  Fixture f;Roles roles;roles.sync(f);SourcePhases phases;phases.sync(f);std::optional<TextureResourceLifecycleView> view;view.emplace(phases.apply(roles.view(f)));
  auto initial=sealedPrepare({},*view);ASSERT_TRUE(ExchangeTextureResourceLifecycle(initial,f.generation,*view));
  const std::array selected{TextureResourceSelection{Domain::Archive,Kind::Texture,0}};const auto cookie=roles.cookies[0];
  phases.aTextures[0]=0;roles.aColor[0]=UINT32_MAX;roles.mapping[0]=-1;view.emplace(phases.apply(roles.view(f)));
  TextureResourceRetirementCommand abandoned;
  {auto canceled=PrepareTextureResourceRetirement(f.generation,*view,selected);abandoned=canceled.retirementCommand();}
  phases.aTextures[0]=1;roles.aColor[0]=0;roles.mapping[0]=0;
  EXPECT_THROW((void)PrepareTextureResourceRetirementReplay(f.generation,phases.apply(roles.view(f)),abandoned,ResourceLifecyclePhase::Active),std::invalid_argument);
  phases.aTextures[0]=0;roles.aColor[0]=UINT32_MAX;roles.mapping[0]=-1;view.emplace(phases.apply(roles.view(f)));
  auto retire=PrepareTextureResourceRetirement(f.generation,*view,selected);const auto command=retire.retirementCommand();retire.seal();ASSERT_TRUE(ExchangeTextureResourceLifecycle(retire,f.generation,*view));
  phases.aTextures[0]=1;roles.aColor[0]=0;roles.mapping[0]=0;roles.cookies[0]=std::make_shared<const uint8_t>(9);
  EXPECT_THROW((void)PrepareTextureResourceRetirementReplay(f.generation,phases.apply(roles.view(f)),command,ResourceLifecyclePhase::Active),std::invalid_argument);
  roles.cookies[0]=cookie;view.emplace(phases.apply(roles.view(f)));auto first=PrepareTextureResourceRetirementReplay(f.generation,*view,command,ResourceLifecyclePhase::Active);
  auto stale=PrepareTextureResourceRetirementReplay(f.generation,*view,command,ResourceLifecyclePhase::Active);first.seal();stale.seal();
  ASSERT_TRUE(ExchangeTextureResourceLifecycle(first,f.generation,*view));EXPECT_FALSE(CanExchangeTextureResourceLifecycle(stale,f.generation,*view));
}

TEST(TextureResourceLifecycle, SelectedArchiveRestoreDoesNotRebindToAFreshEqualEvaluatedMappingTarget) {
  Fixture f;Roles roles;roles.sync(f);SourcePhases phases;phases.sync(f);std::optional<TextureResourceLifecycleView> view;view.emplace(phases.apply(roles.view(f)));
  auto initial=sealedPrepare({},*view);ASSERT_TRUE(ExchangeTextureResourceLifecycle(initial,f.generation,*view));
  const auto mapped=f.generation->resource({Domain::Evaluated,Kind::Texture,0});
  const std::array selected{TextureResourceSelection{Domain::Archive,Kind::Texture,0}};
  phases.aTextures[0]=0;roles.aColor[0]=UINT32_MAX;roles.mapping[0]=-1;view.emplace(phases.apply(roles.view(f)));
  auto retire=PrepareTextureResourceRetirement(f.generation,*view,selected);const auto command=retire.retirementCommand();retire.seal();ASSERT_TRUE(ExchangeTextureResourceLifecycle(retire,f.generation,*view));
  EXPECT_NE(std::ranges::find(command.references()->resources(),mapped),command.references()->resources().end());
  const auto replacement=f.evaluated.textures[0];f.evaluated.textures.clear();phases.eTextures.clear();roles.eColor.clear();roles.eData.clear();
  view.emplace(phases.apply(roles.view(f)));auto trim=sealedReconcile(f.generation,*view);ASSERT_TRUE(ExchangeTextureResourceLifecycle(trim,f.generation,*view));
  f.evaluated.textures.push_back(replacement);phases.eTextures={1};roles.eColor={0};roles.eData={UINT32_MAX};
  view.emplace(phases.apply(roles.view(f)));auto fresh=sealedPrepare(f.generation,*view);ASSERT_TRUE(ExchangeTextureResourceLifecycle(fresh,f.generation,*view));
  EXPECT_NE(f.generation->resource({Domain::Evaluated,Kind::Texture,0}),mapped);
  phases.aTextures[0]=1;roles.aColor[0]=0;roles.mapping[0]=0;
  EXPECT_THROW((void)PrepareTextureResourceRetirementReplay(f.generation,phases.apply(roles.view(f)),command,ResourceLifecyclePhase::Active),std::invalid_argument);
}

TEST(TextureResourceLifecycle, FiveThousandRowsWithAnExplicitSourceHoleStayWithinDeclaredLinearWorkBudget) {
  Fixture f;constexpr size_t count=5000;f.archive.images.resize(count,f.archive.images.front());f.archive.samplers.resize(count);
  f.archive.textures.resize(count,f.archive.textures.front());
  for(size_t i=0;i<count;++i) {f.archive.textures[i].source=int(i);f.archive.textures[i].sampler=int(i);}f.evaluated=f.archive;
  SourcePhases phases;phases.sync(f);phases.aImages[count/2]=phases.aTextures[count/2]=phases.aSamplers[count/2]=0;
  TextureResourceLifecycleLimits limits;limits.maxWork=4'000'000;const auto view=phases.apply(f.view());
  auto candidate=PrepareTextureResourceLifecycle({},view,limits);candidate.seal();ASSERT_TRUE(ExchangeTextureResourceLifecycle(candidate,f.generation,view));
  EXPECT_TRUE(f.generation->matches(view));EXPECT_FALSE(f.generation->active({Domain::Archive,Kind::Texture,uint32_t(count/2)}));
  EXPECT_TRUE(f.generation->active({Domain::Archive,Kind::Texture,uint32_t(count-1)}));
}

TEST(TextureResourceLifecycle, RuntimeOnlyRetirementClearsOnlyItsRolesAndRestoresOriginalHandlesWithoutAllocations) {
  Fixture f;f.append();Roles roles;roles.sync(f);roles.aColor[1]=roles.eColor[1]=1;roles.aData[0]=roles.eData[0]=1;
  const auto initialView=roles.view(f);auto initial=sealedPrepare({},initialView);ASSERT_TRUE(ExchangeTextureResourceLifecycle(initial,f.generation,initialView));
  const auto source=f.generation->resource({Domain::Archive,Kind::Texture,0});const auto runtime=f.generation->resource({Domain::Runtime,Kind::RuntimeTexture,0});
  const auto revision=f.generation->revision();const std::array slots{uint32_t(0)};
  auto runtimeChange=PrepareRuntimeTextureRetirement(f.runtime,slots);const auto runtimeCommand=runtimeChange.command();
  roles.aColor[0]=roles.eColor[0]=UINT32_MAX;const auto retiringView=withRuntime(roles.view(f),runtimeChange.textures());
  const std::array selected{TextureResourceSelection{Domain::Runtime,Kind::RuntimeTexture,0}};
  auto logical=PrepareTextureResourceRetirement(f.generation,retiringView,selected);const auto command=logical.retirementCommand();logical.seal();
  bool logicalPublished{},runtimePublished{};size_t attempts{};
  {material_publication_test::AllocationGuard deny(true);logicalPublished=ExchangeTextureResourceLifecycle(logical,f.generation,retiringView);
    runtimePublished=ExchangeRuntimeTextureRetirement(runtimeChange,f.runtime);attempts=deny.allocations();}
  ASSERT_TRUE(logicalPublished);ASSERT_TRUE(runtimePublished);EXPECT_EQ(attempts,0u);EXPECT_EQ(f.generation->revision(),revision+1);
  EXPECT_TRUE(f.generation->active({Domain::Archive,Kind::Texture,0}));EXPECT_EQ(f.generation->resource({Domain::Archive,Kind::Texture,0}),source);
  EXPECT_FALSE(f.generation->active(selected[0]));EXPECT_EQ(roles.aData[0],1u);EXPECT_EQ(roles.mapping[0],0);EXPECT_TRUE(f.generation->matches(roles.view(f)));
  // A later independent role edit and runtime append survive exact role-only
  // replay; its opposite role is outside the selected command's authority.
  TextureResource newer;newer.name="new-opposite-role";newer.contentKey="new-opposite-role";ASSERT_EQ(f.runtime.registerTexture(newer),2u);
  roles.aData[0]=roles.eData[0]=2;const auto latestView=roles.view(f);auto latest=sealedPrepare(f.generation,latestView);
  ASSERT_TRUE(ExchangeTextureResourceLifecycle(latest,f.generation,latestView));
  auto runtimeRestore=PrepareRuntimeTextureRetirementReplay(f.runtime,runtimeCommand,RuntimeTextureRetirementTarget::Active);
  roles.aColor[0]=roles.eColor[0]=0;const auto restoringView=withRuntime(roles.view(f),runtimeRestore.textures());
  auto restore=PrepareTextureResourceRetirementReplay(f.generation,restoringView,command,ResourceLifecyclePhase::Active);restore.seal();
  {material_publication_test::AllocationGuard deny(true);logicalPublished=ExchangeTextureResourceLifecycle(restore,f.generation,restoringView);
    runtimePublished=ExchangeRuntimeTextureRetirement(runtimeRestore,f.runtime);attempts=deny.allocations();}
  ASSERT_TRUE(logicalPublished);ASSERT_TRUE(runtimePublished);EXPECT_EQ(attempts,0u);EXPECT_EQ(f.generation->resource(selected[0]),runtime);
  EXPECT_EQ(roles.aData[0],2u);EXPECT_EQ(roles.eData[0],2u);EXPECT_EQ(roles.aColor[1],1u);EXPECT_TRUE(f.generation->matches(roles.view(f)));
}

TEST(TextureResourceLifecycle, ExplicitSourceAndRuntimeSelectionRetiresAndRestoresInOneCatalogMutation) {
  Fixture f;Roles roles;roles.sync(f);SourcePhases phases;phases.sync(f);const auto initialView=phases.apply(roles.view(f));
  auto initial=sealedPrepare({},initialView);ASSERT_TRUE(ExchangeTextureResourceLifecycle(initial,f.generation,initialView));
  const auto revision=f.generation->revision();const std::array slots{uint32_t(0)};auto runtime=PrepareRuntimeTextureRetirement(f.runtime,slots);const auto runtimeCommand=runtime.command();
  phases.aTextures[0]=phases.eTextures[0]=0;roles.aColor[0]=roles.eColor[0]=UINT32_MAX;roles.mapping[0]=-1;
  const auto retiringView=phases.apply(withRuntime(roles.view(f),runtime.textures()));
  const std::array selected{TextureResourceSelection{Domain::Archive,Kind::Texture,0},TextureResourceSelection{Domain::Evaluated,Kind::Texture,0},
      TextureResourceSelection{Domain::Runtime,Kind::RuntimeTexture,0}};
  auto logical=PrepareTextureResourceRetirement(f.generation,retiringView,selected);const auto command=logical.retirementCommand();logical.seal();
  ASSERT_TRUE(ExchangeTextureResourceLifecycle(logical,f.generation,retiringView));ASSERT_TRUE(ExchangeRuntimeTextureRetirement(runtime,f.runtime));
  EXPECT_EQ(f.generation->revision(),revision+1);for(const auto selection:selected) EXPECT_FALSE(f.generation->active(selection));
  auto runtimeRestore=PrepareRuntimeTextureRetirementReplay(f.runtime,runtimeCommand,RuntimeTextureRetirementTarget::Active);
  phases.aTextures[0]=phases.eTextures[0]=1;roles.aColor[0]=roles.eColor[0]=0;roles.mapping[0]=0;
  const auto restoringView=phases.apply(withRuntime(roles.view(f),runtimeRestore.textures()));
  auto restore=PrepareTextureResourceRetirementReplay(f.generation,restoringView,command,ResourceLifecyclePhase::Active);restore.seal();
  ASSERT_TRUE(ExchangeTextureResourceLifecycle(restore,f.generation,restoringView));ASSERT_TRUE(ExchangeRuntimeTextureRetirement(runtimeRestore,f.runtime));
  EXPECT_EQ(f.generation->revision(),revision+2);for(const auto selection:selected) EXPECT_TRUE(f.generation->active(selection));
}

TEST(TextureResourceLifecycle, RuntimeRetirementStillRejectsResidualMaterialDependencyAfterOwnedRoleCleanup) {
  Fixture f;Roles roles;roles.sync(f);const auto initialView=roles.view(f);auto initial=sealedPrepare({},initialView);ASSERT_TRUE(ExchangeTextureResourceLifecycle(initial,f.generation,initialView));
  ResourceLifecycleCatalog materials;const auto domain=materials.createDomain(Domain::Runtime);const auto material=materials.issue(domain,Kind::Material,0);
  const std::array selected{TextureResourceSelection{Domain::Runtime,Kind::RuntimeTexture,0}};
  auto bind=PrepareTextureResourceLifecycle(f.generation,roles.view(f));auto owner=bind.stageLiveOwner({material,TextureResourceLiveOwnerKind::Material,0},selected);
  bind.seal();ASSERT_TRUE(ExchangeTextureResourceLifecycle(bind,f.generation,roles.view(f)));
  const std::array slots{uint32_t(0)};auto runtime=PrepareRuntimeTextureRetirement(f.runtime,slots);roles.aColor[0]=roles.eColor[0]=UINT32_MAX;
  const auto retiringView=withRuntime(roles.view(f),runtime.textures());auto logical=PrepareTextureResourceRetirement(f.generation,retiringView,selected);
  EXPECT_THROW(logical.seal(),std::invalid_argument);EXPECT_TRUE(owner->active());EXPECT_TRUE(f.runtime.isTextureActive(0));
  EXPECT_FALSE(runtime.command().published());EXPECT_FALSE(logical.retirementCommand().references()->resources().empty());
}

TEST(TextureResourceLifecycle, RuntimeRestoreRejectsAReplacementCapabilityAtTheSameNumericSlot) {
  Fixture f;f.publish();const std::array slots{uint32_t(0)};auto runtime=PrepareRuntimeTextureRetirement(f.runtime,slots);
  const auto retiringView=withRuntime(f.view(),runtime.textures());const std::array selected{TextureResourceSelection{Domain::Runtime,Kind::RuntimeTexture,0}};
  auto logical=PrepareTextureResourceRetirement(f.generation,retiringView,selected);const auto command=logical.retirementCommand();logical.seal();
  ASSERT_TRUE(ExchangeTextureResourceLifecycle(logical,f.generation,retiringView));ASSERT_TRUE(ExchangeRuntimeTextureRetirement(runtime,f.runtime));
  TextureManager replacement;TextureResource different;different.name="different capability";different.contentKey="different";different.width=41;
  (void)replacement.registerTexture(different);const auto wrongView=withRuntime(f.view(),replacement);
  EXPECT_THROW((void)PrepareTextureResourceRetirementReplay(f.generation,wrongView,command,ResourceLifecyclePhase::Active),std::invalid_argument);
  EXPECT_FALSE(f.generation->active(selected[0]));
}

TEST(TextureResourceLifecycle, OriginalAppendSuspensionAndResumePreserveAnEarlierSelectedDeleteEndpoint) {
  Fixture f;f.publish();f.append();auto imported=sealedPrepare(f.generation,f.view());const auto import=imported.command();
  ASSERT_TRUE(ExchangeTextureResourceLifecycle(imported,f.generation,f.view()));
  const std::array selected{TextureResourceSelection{Domain::Archive,Kind::Texture,1}};
  const auto original=f.generation->resource(selected[0]);SourcePhases phases;phases.sync(f);phases.aTextures[1]=0;
  const auto removedView=phases.apply(f.view());auto deletion=PrepareTextureResourceRetirement(f.generation,removedView,selected);
  const auto originalDelete=deletion.retirementCommand();deletion.seal();ASSERT_TRUE(ExchangeTextureResourceLifecycle(deletion,f.generation,removedView));
  phases.aTextures[1]=1;const auto restoredView=phases.apply(f.view());
  auto undoDelete=PrepareTextureResourceRetirementReplay(f.generation,restoredView,originalDelete,ResourceLifecyclePhase::Active);undoDelete.seal();
  ASSERT_TRUE(ExchangeTextureResourceLifecycle(undoDelete,f.generation,restoredView));
  f.trim();auto cut=sealedReplay(f.generation,f.view(),import,false);ASSERT_TRUE(ExchangeTextureResourceLifecycle(cut,f.generation,f.view()));
  f.append();auto restoreImport=sealedReplay(f.generation,f.view(),import,true);ASSERT_TRUE(ExchangeTextureResourceLifecycle(restoreImport,f.generation,f.view()));
  EXPECT_EQ(f.generation->resource(selected[0]),original);phases.sync(f);phases.aTextures[1]=0;const auto redoView=phases.apply(f.view());
  auto redoDelete=PrepareTextureResourceRetirementReplay(f.generation,redoView,originalDelete,ResourceLifecyclePhase::Retired);redoDelete.seal();
  ASSERT_TRUE(ExchangeTextureResourceLifecycle(redoDelete,f.generation,redoView));EXPECT_FALSE(f.generation->active(selected[0]));
}
}
