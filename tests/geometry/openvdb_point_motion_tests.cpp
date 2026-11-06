#include "Container/geometry/OpenVdbPointMotion.h"
#include <gtest/gtest.h>
#include <atomic>
#include <bit>
#include <chrono>
#include <fstream>
#include <map>
#include <limits>

#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
#include <openvdb/io/File.h>
#include <openvdb/points/PointConversion.h>
#include <openvdb/points/PointGroup.h>
#include <openvdb/points/PointAttribute.h>
#include <sstream>
#endif

namespace {
namespace vdb=container::geometry::vdb;
TEST(OpenVdbPointMotionAvailability, DisabledBeforeEveryPathAndOption) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  GTEST_SKIP();
#else
  vdb::PointMotionOptions o;o.worldFromWorld[15]=0;o.maxWork=0;
  vdb::ProcessingOptions p;p.maxInputBytes=0;
  try {(void)vdb::WriteMovedPoints("missing","missing/out",o,{},p);FAIL();}
  catch(const std::runtime_error& e){EXPECT_NE(std::string(e.what()).find("ENABLE_OPENVDB=ON"),std::string::npos);}
#endif
}
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
namespace points=openvdb::points;
using Grid=points::PointDataGrid;
using Leaf=Grid::TreeType::LeafNodeType;
using V=openvdb::Vec3d;
struct Oracle {
  V world;
  int64_t id;
  bool a,b,active;
  std::string text;
  std::array<int32_t,3> stride;
  uint64_t floating;
  openvdb::Vec4d custom;
  openvdb::Vec3f velocity;
  uint32_t truncated;
};
class OpenVdbPointMotion:public ::testing::Test {
protected:
  void SetUp()override {
    openvdb::initialize();
    reg<int64_t>();reg<double>();reg<openvdb::Vec4d>();
    using Truncated=points::TypedAttributeArray<float,points::TruncateCodec>;
    if(!Truncated::isRegistered())Truncated::registerType();
    if(!points::StringAttributeArray::isRegistered())points::StringAttributeArray::registerType();
    static std::atomic_uint64_t serial{};
    directory=std::filesystem::temp_directory_path()/("container-point-motion-"+
      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+"-"+std::to_string(++serial));
    std::filesystem::create_directory(directory);p.gridName="moved";
    auto grid=create();grid->tree().setActiveState({-16,0,0},false);
    openvdb::MetaMap meta;meta.insertMeta("file:binary",openvdb::StringMetadata(std::string("a\0b",3)));
    write("source.vdb",grid,meta);
  }
  template<class T>void reg(){using A=points::TypedAttributeArray<T>;if(!A::isRegistered())A::registerType();}
  void TearDown()override {std::error_code e;for(const auto& f:std::filesystem::directory_iterator(directory))std::filesystem::remove_all(f.path(),e);std::filesystem::remove(directory,e);}
  std::filesystem::path path(const std::string& n)const{return directory/n;}
  std::string bytes(const std::filesystem::path& n){std::ifstream file(n,std::ios::binary);return {std::istreambuf_iterator<char>(file),{}};}
  template<class Codec=points::NullCodec>Grid::Ptr create(const std::vector<V>& positions={{-16.2,0,0},{-16.1,0,0},{.1,0,0},{.2,0,0},{4,0,0},{32,0,0}}) {
    auto transform=openvdb::math::Transform::createLinearTransform(1);
    auto grid=points::createPointDataGrid<Codec,Grid>(positions,*transform);grid->setName("points");
    points::appendAttribute<int64_t>(grid->tree(),"id");
    points::appendAttribute<double>(grid->tree(),"raw");
    points::appendAttribute<openvdb::Vec4d>(grid->tree(),"custom4");
    points::appendAttribute<float,points::TruncateCodec>(grid->tree(),"encoded_half");
    points::appendAttribute<openvdb::Vec3f>(grid->tree(),"v",openvdb::Vec3f(3,4,5));
    points::appendAttribute<float>(grid->tree(),"pscale",2);
    points::appendAttribute<int32_t>(grid->tree(),"triple",7,3);
    points::appendAttribute(grid->tree(),"label",points::StringAttributeArray::attributeType());
    points::appendGroup(grid->tree(),"A");points::appendGroup(grid->tree(),"B");points::appendGroup(grid->tree(),"empty");
    auto descriptor=points::makeDescriptorUnique(grid->tree());
    descriptor->getMetadata().insertMeta("custom:descriptor",openvdb::StringMetadata(std::string("opaque\0bytes",12)));
    {points::StringMetaInserter dictionary(descriptor->getMetadata());dictionary.insert("alpha");dictionary.insert(std::string("beta\0raw",8));}
    std::vector<Leaf*> leaves;for(auto leaf=grid->tree().beginLeaf();leaf;++leaf)leaves.push_back(&*leaf);
    std::sort(leaves.begin(),leaves.end(),[](const auto* a,const auto* b){return a->origin()<b->origin();});
    uint64_t ordinal=0;
    for(auto* leaf:leaves) {
      points::AttributeWriteHandle<int64_t> ids(leaf->attributeArray("id"));
      points::AttributeWriteHandle<double> raw(leaf->attributeArray("raw"));
      points::AttributeWriteHandle<openvdb::Vec4d> custom(leaf->attributeArray("custom4"));
      points::AttributeWriteHandle<float> truncated(leaf->attributeArray("encoded_half"));
      points::StringAttributeWriteHandle text(leaf->attributeArray("label"),leaf->attributeSet().descriptor().getMetadata());
      auto a=leaf->groupWriteHandle("A"),b=leaf->groupWriteHandle("B");
      for(auto i=leaf->beginIndexAll();i;++i,++ordinal) {
        ids.set(*i,INT64_MAX-int64_t(ordinal));
        raw.set(*i,std::bit_cast<double>(ordinal%3==0?UINT64_C(0x8000000000000000):ordinal%3==1?UINT64_C(0x7ff8000000000123):UINT64_C(0x3ff0000000000000)));
        custom.set(*i,openvdb::Vec4d(double(ordinal),-double(ordinal),.125,.5));
        truncated.set(*i,ordinal%2?-.125f:.375f);
        text.set(*i,ordinal%2?std::string("beta\0raw",8):"alpha");a.set(*i,ordinal%2==0);b.set(*i,ordinal<4);
      }
      leaf->attributeArray("id").setHidden(true);leaf->attributeArray("custom4").setHidden(true);
    }
    grid->insertMeta("artist",openvdb::StringMetadata(std::string("raw\0artist",10)));
    return grid;
  }
  void write(const std::string& n,Grid::ConstPtr grid,const openvdb::MetaMap& meta={}) {
    openvdb::io::File writer(path(n).string());writer.setCompression(openvdb::io::COMPRESS_ZIP);writer.write({grid},meta);writer.close();
  }
  Grid::Ptr read(const std::string& n,const std::string& grid="points") {
    openvdb::io::File reader(path(n).string());reader.open(false);auto result=openvdb::gridPtrCast<Grid>(reader.readGrid(grid));reader.close();return result;
  }
  std::map<int64_t,Oracle> oracle(Grid::ConstPtr grid) {
    std::map<int64_t,Oracle> result;
    for(auto leaf=grid->tree().cbeginLeaf();leaf;++leaf) {
      points::AttributeHandle<openvdb::Vec3f> positions(leaf->constAttributeArray("P"));
      points::AttributeHandle<int64_t> ids(leaf->constAttributeArray("id"));
      points::AttributeHandle<double> raw(leaf->constAttributeArray("raw"));
      points::AttributeHandle<openvdb::Vec4d> custom(leaf->constAttributeArray("custom4"));
      points::AttributeHandle<openvdb::Vec3f> velocity(leaf->constAttributeArray("v"));
      points::AttributeHandle<float> truncated(leaf->constAttributeArray("encoded_half"));
      points::AttributeHandle<int32_t> triples(leaf->constAttributeArray("triple"));
      points::StringAttributeHandle text(leaf->constAttributeArray("label"),leaf->attributeSet().descriptor().getMetadata());
      auto a=leaf->groupHandle("A"),b=leaf->groupHandle("B");
      for(auto i=leaf->beginIndexAll();i;++i) {
        const auto id=ids.get(*i);const auto local=positions.get(*i);
        const auto world=grid->transform().indexToWorld(i.getCoord().asVec3d()+V(local));
        result.emplace(id,Oracle{world,id,a.get(*i),b.get(*i),leaf->isValueOn(i.getCoord()),text.get(*i),
          {triples.get(*i,0),triples.get(*i,1),triples.get(*i,2)},std::bit_cast<uint64_t>(raw.get(*i)),custom.get(*i),velocity.get(*i),std::bit_cast<uint32_t>(truncated.get(*i))});
      }
    }
    return result;
  }
  void comparePayload(const std::map<int64_t,Oracle>& before,const std::map<int64_t,Oracle>& after) {
    ASSERT_EQ(before.size(),after.size());for(const auto& [id,a]:before) {
      ASSERT_TRUE(after.contains(id));const auto& b=after.at(id);
      EXPECT_EQ(a.id,b.id);EXPECT_EQ(a.a,b.a);EXPECT_EQ(a.b,b.b);EXPECT_EQ(a.text,b.text);EXPECT_EQ(a.stride,b.stride);
      EXPECT_EQ(a.floating,b.floating);EXPECT_EQ(a.custom,b.custom);EXPECT_EQ(a.velocity,b.velocity);EXPECT_EQ(a.truncated,b.truncated);
    }
  }
  auto run(const std::string& input="source.vdb",const std::string& output="out.vdb") {return vdb::WriteMovedPoints(path(input),path(output),o,{},p,limits);}
  std::filesystem::path directory;vdb::PointMotionOptions o;vdb::ProcessingOptions p;vdb::PointAttributeOptions limits;
};

TEST_F(OpenVdbPointMotion, NativeTranslationPreservesArbitraryPayloadGroupsInactiveRecordsAndMetadata) {
  const auto source=bytes(path("source.vdb"));auto before=oracle(read("source.vdb"));o.worldFromWorld[3]=20;
  const auto result=run();EXPECT_FALSE(result.noOp);EXPECT_EQ(result.inputCount,6);EXPECT_EQ(result.selectedCount,6);EXPECT_EQ(result.outputCount,6);
  EXPECT_EQ(result.inputActiveCount,4);EXPECT_EQ(result.outputActiveCount,4);EXPECT_EQ(result.inactiveRecordsActivated,0);
  auto target=read("out.vdb","moved");auto after=oracle(target);comparePayload(before,after);
  for(const auto& [id,a]:before) {const auto& b=after.at(id);EXPECT_NEAR(b.world[0],a.world[0]+20,1e-5);EXPECT_EQ(a.active,b.active);}
  EXPECT_EQ(source,bytes(path("source.vdb")));EXPECT_EQ((*target)["artist"]->str(),std::string("raw\0artist",10));
  openvdb::io::File reader(path("out.vdb").string());reader.open(false);EXPECT_EQ((*reader.getMetadata())["file:binary"]->str(),std::string("a\0b",3));reader.close();
  for(auto leaf=target->tree().cbeginLeaf();leaf;++leaf) {
    EXPECT_TRUE(leaf->constAttributeArray("id").isHidden());EXPECT_TRUE(leaf->constAttributeArray("custom4").isHidden());
    EXPECT_EQ(leaf->attributeSet().descriptor().getMetadata()["custom:descriptor"]->str(),std::string("opaque\0bytes",12));
    EXPECT_FALSE(leaf->hasAttribute("container_p05_original_record"));
  }
}
TEST_F(OpenVdbPointMotion, GroupSelectedWorldAffineUsesColumnConventionAndLeavesOtherPositionsAndVelocity) {
  const auto before=oracle(read("source.vdb"));o.groups.anyOf={"A"};o.groups.allOf={"B"};o.groups.noneOf={"empty"};
  o.worldFromWorld={0,-2,0,3, 1,0,.25,-4, 0,0,-1,2, 0,0,0,1};
  const auto result=run();EXPECT_EQ(result.selectedCount,2);auto after=oracle(read("out.vdb","moved"));comparePayload(before,after);
  for(const auto& [id,a]:before) {
    const auto& b=after.at(id);const V expected=a.a&&a.b?V(-2*a.world[1]+3,a.world[0]+.25*a.world[2]-4,-a.world[2]+2):a.world;
    for(size_t c=0;c<3;++c)EXPECT_NEAR(b.world[c],expected[c],1e-5);
  }
  EXPECT_LE(result.maxUnselectedPositionErrorVoxels,1e-6);
}
TEST_F(OpenVdbPointMotion, TargetMapRepartitionsUnselectedPointsAndPreservesExactRotatedReflectedCoefficients) {
  auto before=oracle(read("source.vdb"));o.groups.anyOf={"empty"};
  const double c=std::cos(.3)*.5,s=std::sin(.3)*.5;
  o.targetIndexToWorld=std::array<double,16>{c,-s,0,1.25, s,c,0,-2.5, 0,0,-.5,3, 0,0,0,1};
  const auto result=run();EXPECT_FALSE(result.noOp);EXPECT_EQ(result.selectedCount,0);
  auto target=read("out.vdb","moved");auto after=oracle(target);comparePayload(before,after);
  for(const auto& [id,a]:before)for(size_t d=0;d<3;++d)EXPECT_NEAR(after.at(id).world[d],a.world[d],2e-6);
  const auto origin=target->transform().indexToWorld(V(0));EXPECT_EQ(origin,V(1.25,-2.5,3));
  EXPECT_EQ(target->transform().baseMap()->applyJacobian({1,0,0}),V(c,s,0));
  EXPECT_EQ(target->transform().baseMap()->applyJacobian({0,1,0}),V(-s,c,0));
}
TEST_F(OpenVdbPointMotion, InactiveActivityCollisionRejectsByDefaultAndExplicitOrCountsEveryAffectedRecord) {
  auto grid=create({{0,0,0},{8,0,0}});grid->tree().setActiveState({0,0,0},false);write("collision.vdb",grid);
  o.worldFromWorld={0,0,0,1, 0,0,0,0, 0,0,0,0, 0,0,0,1};
  EXPECT_THROW(run("collision.vdb"),std::invalid_argument);EXPECT_FALSE(std::filesystem::exists(path("out.vdb")));
  o.rejectActivityMerges=false;const auto result=run("collision.vdb");EXPECT_EQ(result.inactiveRecordsActivated,1);EXPECT_EQ(result.outputActiveCount,2);
  auto after=oracle(read("out.vdb","moved"));EXPECT_EQ(after.size(),2);for(const auto& [id,point]:after){(void)id;EXPECT_TRUE(point.active);EXPECT_EQ(point.world,V(1,0,0));}
}
TEST_F(OpenVdbPointMotion, Fixed16CodecIsRetainedAndFixed8NeedsExplicitQuantizationTolerance) {
  auto grid=create<points::FixedPointCodec<false>>();write("fixed16.vdb",grid);o.worldFromWorld[3]=.12345;
  const auto before=oracle(grid);const auto result=run("fixed16.vdb");EXPECT_GT(result.maxPositionErrorVoxels,0);EXPECT_LE(result.maxPositionErrorVoxels,1e-4);
  auto target=read("out.vdb","moved");for(auto leaf=target->tree().cbeginLeaf();leaf;++leaf)EXPECT_EQ(leaf->constAttributeArray("P").type().second,"fxpt16");
  auto after=oracle(target);comparePayload(before,after);for(const auto& [id,a]:before)EXPECT_NEAR(after.at(id).world[0],a.world[0]+.12345,1e-4);
  auto coarse=create<points::FixedPointCodec<true>>();write("fixed8.vdb",coarse);
  o.worldFromWorld[3]=0;const auto unchanged=run("fixed8.vdb","fixed8-copy.vdb");
  EXPECT_TRUE(unchanged.noOp);EXPECT_EQ(unchanged.maxPositionErrorVoxels,0);
  auto exactCopy=read("fixed8-copy.vdb","moved");
  for(auto leaf=coarse->tree().cbeginLeaf();leaf;++leaf)
    EXPECT_EQ(leaf->constAttributeArray("P"),exactCopy->tree().probeConstLeaf(leaf->origin())->constAttributeArray("P"));
  o.worldFromWorld[3]=.12345;
  EXPECT_THROW(run("fixed8.vdb","bad.vdb"),std::invalid_argument);EXPECT_FALSE(std::filesystem::exists(path("bad.vdb")));
  o.maxPositionErrorVoxels=.01;const auto coarseResult=run("fixed8.vdb","coarse.vdb");EXPECT_GT(coarseResult.maxPositionErrorVoxels,1e-4);EXPECT_LE(coarseResult.maxPositionErrorVoxels,.01);
}
TEST_F(OpenVdbPointMotion, ExactNoopRetainsDynamicLayoutsAndMovementRejectsThem) {
  auto grid=create();points::appendAttribute<int32_t>(grid->tree(),"dynamic",7,20,false);write("dynamic.vdb",grid);
  const auto result=run("dynamic.vdb");EXPECT_TRUE(result.noOp);EXPECT_EQ(result.maxPositionErrorVoxels,0);
  auto target=read("out.vdb","moved");auto source=read("dynamic.vdb");
  for(auto leaf=target->tree().cbeginLeaf();leaf;++leaf)EXPECT_EQ(leaf->constAttributeArray("dynamic"),source->tree().probeConstLeaf(leaf->origin())->constAttributeArray("dynamic"));
  o.worldFromWorld[3]=1;EXPECT_THROW(run("dynamic.vdb","bad.vdb"),std::invalid_argument);EXPECT_FALSE(std::filesystem::exists(path("bad.vdb")));
}
TEST_F(OpenVdbPointMotion, EmptyAndArchivedSchemaNoopsRetainDescriptorArchiveAndFileMetadata) {
  auto empty=Grid::create();empty->setName("points");write("empty.vdb",empty);const auto result=run("empty.vdb");EXPECT_TRUE(result.noOp);EXPECT_EQ(result.outputCount,0);
  vdb::PointGroupSelection none;none.anyOf={"empty"};auto compact=vdb::WriteSelectedPoints(path("source.vdb"),path("archived.vdb"),none,vdb::PointSelectionMode::KeepSelected,{},p,limits);
  ASSERT_TRUE(compact.archivedSchema);const auto archived=run("archived.vdb","archive-copy.vdb");EXPECT_TRUE(archived.archivedSchema);EXPECT_TRUE(archived.noOp);EXPECT_EQ(archived.schema.groups,compact.schema.groups);
  auto counted=vdb::CountSelectedPoints(path("archive-copy.vdb"),{},"moved",p,limits);EXPECT_TRUE(counted.archivedSchema);EXPECT_EQ(counted.schema.attributes.size(),compact.schema.attributes.size());
}
TEST_F(OpenVdbPointMotion, TinyNativeUniformMapsAndLargeRemoteIndicesAreExplicitlyConditioned) {
  auto grid=create();
  std::stringstream record(std::ios::in|std::ios::out|std::ios::binary);
  openvdb::writeString(record,openvdb::math::UniformScaleMap::mapType());
  for(double x:{1e-6,1e-6,1e6,1e12,.5e6})V(x).write(record);
  record.seekg(0);openvdb::io::setCurrentVersion(record);auto map=std::make_shared<openvdb::math::Transform>();map->read(record);grid->setTransform(map);
  // Archive::writeGrid's simplifying copy is bypassed locally for this fixture.
  class Exact:public Grid {public:explicit Exact(const Grid& g):Grid(openvdb::ConstPtrCast<Grid::TreeType>(g.constTreePtr())){insertMeta(g);setTransform(std::make_shared<openvdb::math::Transform>(g.transform()));}
    openvdb::GridBase::ConstPtr copyGrid()const override{return copyReplacingMetadataAndTransform(*this,std::make_shared<openvdb::math::Transform>(transform()));}
    openvdb::GridBase::Ptr copyGrid()override{return openvdb::ConstPtrCast<openvdb::GridBase>(static_cast<const Exact&>(*this).copyGrid());}};
  write("tiny.vdb",std::make_shared<Exact>(*grid));o.worldFromWorld[3]=3e-6;
  const auto result=run("tiny.vdb");EXPECT_FALSE(result.noOp);auto target=read("out.vdb","moved");EXPECT_EQ(target->transform().baseMap()->applyJacobian({1,0,0}),V(1e-6,0,0));
  auto original=oracle(grid),moved=oracle(target);for(const auto& [id,a]:original)EXPECT_NEAR(moved.at(id).world[0],a.world[0]+3e-6,1e-10);
  o.targetIndexToWorld=std::array<double,16>{1e-6,0,0,2e-6, 0,1e-6,0,0, 0,0,1e-6,0, 0,0,0,1};
  const auto remap=run("tiny.vdb","tiny-map.vdb");EXPECT_FALSE(remap.noOp);auto mapped=read("tiny-map.vdb","moved");EXPECT_EQ(mapped->transform().indexToWorld(V(0)),V(2e-6,0,0));
  o.targetIndexToWorld.reset();o.worldFromWorld[3]=1e15;EXPECT_THROW(run("source.vdb","bad.vdb"),std::invalid_argument);EXPECT_FALSE(std::filesystem::exists(path("bad.vdb")));
}
TEST_F(OpenVdbPointMotion, PersistentFlagMismatchRejectsMovementButExactNoopKeepsNativeFlags) {
  auto grid=create();grid->tree().beginLeaf()->attributeArray("id").setHidden(false);write("flags.vdb",grid);
  EXPECT_TRUE(run("flags.vdb").noOp);o.worldFromWorld[3]=1;EXPECT_THROW(run("flags.vdb","bad.vdb"),std::invalid_argument);
  o.worldFromWorld[3]=0;o.groups.anyOf={"missing"};EXPECT_THROW(run("source.vdb","bad.vdb"),std::invalid_argument);
}
TEST_F(OpenVdbPointMotion, RemoteIndexRepartitionAndUnsafeWorldArithmeticHaveIndependentBounds) {
  auto grid=create({{1e9+.125,0,0},{1e9+16.25,0,0}});write("remote.vdb",grid);
  o.worldFromWorld[3]=16.5;const auto result=run("remote.vdb");EXPECT_FALSE(result.noOp);
  const auto before=oracle(grid),after=oracle(read("out.vdb","moved"));comparePayload(before,after);
  for(const auto& [id,a]:before)EXPECT_NEAR(after.at(id).world[0],a.world[0]+16.5,1e-4);
  o.worldFromWorld[3]=1e-100;
  EXPECT_THROW(run("source.vdb","unresolved.vdb"),std::invalid_argument);EXPECT_FALSE(std::filesystem::exists(path("unresolved.vdb")));
  auto cancellation=create({{1,1,0}});write("cancel.vdb",cancellation);
  o.worldFromWorld={1e100,-1e100,0,1, 0,1,0,0, 0,0,1,0, 0,0,0,1};
  EXPECT_THROW(run("cancel.vdb","cancel-output.vdb"),std::invalid_argument);EXPECT_FALSE(std::filesystem::exists(path("cancel-output.vdb")));
}
TEST_F(OpenVdbPointMotion, CapsInvalidMapsAndUnrepresentableCoordinatesNeverPublish) {
  for(int which=0;which<7;++which) {
    o={};p={};p.gridName="moved";limits={};
    if(which==0)o.maxWork=1;if(which==1)o.maxMetadataBytes=1;if(which==2)p.maxInputBytes=1024;
    if(which==3)p.maxVoxelCount=512;if(which==4)limits.maxPoints=1;if(which==5)limits.maxAttributes=2;if(which==6)limits.maxStringBytes=1;
    EXPECT_THROW(run(),std::invalid_argument)<<which;EXPECT_FALSE(std::filesystem::exists(path("out.vdb")));
  }
  o={};p={};p.gridName="moved";limits={};
  o.worldFromWorld[15]=0;EXPECT_THROW(run(),std::invalid_argument);
  o={};o.targetIndexToWorld=std::array<double,16>{1,0,0,0, 0,2,0,0, 0,0,1,0, 0,0,0,1};EXPECT_THROW(run(),std::invalid_argument);
  o={};o.worldFromWorld[3]=double(INT32_MAX);EXPECT_THROW(run(),std::invalid_argument);EXPECT_FALSE(std::filesystem::exists(path("out.vdb")));
}
TEST_F(OpenVdbPointMotion, UnicodeParentsExclusiveOutputsAndSourceBytesRemainIntact) {
  const auto unicode=directory/std::filesystem::path(std::u8string(u8"移動-é"));std::filesystem::create_directory(unicode);
  const auto source=unicode/std::filesystem::path(std::u8string(u8"入力.vdb")),output=unicode/std::filesystem::path(std::u8string(u8"点.vdb"));
  std::filesystem::copy_file(path("source.vdb"),source);const auto original=bytes(source);o.worldFromWorld[3]=3;
  auto result=vdb::WriteMovedPoints(source,output,o,{},p,limits);EXPECT_EQ(result.outputCount,6);const auto completed=bytes(output);
  EXPECT_THROW(vdb::WriteMovedPoints(source,output,o,{},p,limits),std::invalid_argument);EXPECT_EQ(completed,bytes(output));EXPECT_EQ(original,bytes(source));
  EXPECT_THROW(vdb::WriteMovedPoints(source,source,o,{},p,limits),std::invalid_argument);EXPECT_EQ(original,bytes(source));
}
TEST_F(OpenVdbPointMotion, DisabledDelayedLoadingFailsBeforeNativePayloadMaterialization) {
  struct Restore {
    std::string value;bool present=false;
    Restore(){const char* x=std::getenv("OPENVDB_DISABLE_DELAYED_LOAD");present=x;if(x)value=x;
#ifdef _WIN32
      _putenv_s("OPENVDB_DISABLE_DELAYED_LOAD","1");
#else
      setenv("OPENVDB_DISABLE_DELAYED_LOAD","1",1);
#endif
    }
    ~Restore(){
#ifdef _WIN32
      _putenv_s("OPENVDB_DISABLE_DELAYED_LOAD",present?value.c_str():"");
#else
      if(present)setenv("OPENVDB_DISABLE_DELAYED_LOAD",value.c_str(),1);else unsetenv("OPENVDB_DISABLE_DELAYED_LOAD");
#endif
    }
  }restore;
  try{(void)run();FAIL();}catch(const std::invalid_argument& e){EXPECT_NE(std::string(e.what()).find("delayed loading"),std::string::npos);}
  EXPECT_FALSE(std::filesystem::exists(path("out.vdb")));
}
#endif
} // namespace
