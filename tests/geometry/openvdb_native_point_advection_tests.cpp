#include "Container/geometry/OpenVdbNativePointAdvection.h"
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
TEST(OpenVdbNativePointAdvectionAvailability, DisabledBeforeEveryPathAndOption) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  GTEST_SKIP();
#else
  vdb::NativePointAdvectionOptions o;o.timeStep=INFINITY;o.maxWork=0;
  vdb::ProcessingOptions p;p.maxInputBytes=0;
  try {(void)vdb::WriteAdvectedNativePoints("missing","missing-velocity","missing/out",o,{},{},p);FAIL();}
  catch(const std::runtime_error& e){EXPECT_NE(std::string(e.what()).find("ENABLE_OPENVDB=ON"),std::string::npos);}
#endif
}
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
namespace points=openvdb::points;
using Grid=points::PointDataGrid;
using Leaf=Grid::TreeType::LeafNodeType;
using V=openvdb::Vec3d;
using C=openvdb::Coord;
double length(const V& value){return std::hypot(value.x(),value.y(),value.z());}
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
class OpenVdbNativePointAdvection:public ::testing::Test {
protected:
  void SetUp()override {
    openvdb::initialize();
    reg<int64_t>();reg<double>();reg<openvdb::Vec4d>();
    using Truncated=points::TypedAttributeArray<float,points::TruncateCodec>;
    if(!Truncated::isRegistered())Truncated::registerType();
    if(!points::StringAttributeArray::isRegistered())points::StringAttributeArray::registerType();
    static std::atomic_uint64_t serial{};
    directory=std::filesystem::temp_directory_path()/("container-native-point-advection-"+
      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+"-"+std::to_string(++serial));
    std::filesystem::create_directory(directory);p.gridName="moved";o.timeStep=1;
    auto velocity=openvdb::Vec3DGrid::create(V(.5,0,0));world(*velocity);field("velocity.vdb",velocity);
    auto grid=create();grid->tree().setActiveState({-16,0,0},false);
    openvdb::MetaMap meta;meta.insertMeta("file:binary",openvdb::StringMetadata(std::string("a\0b",3)));
    write("source.vdb",grid,meta);
  }
  template<class T>void reg(){using A=points::TypedAttributeArray<T>;if(!A::isRegistered())A::registerType();}
  void TearDown()override {
    std::error_code e;
    if(std::filesystem::is_symlink(directory,e) || e)return;
    const auto target=std::filesystem::weakly_canonical(directory,e);if(e)return;
    const auto parent=std::filesystem::weakly_canonical(std::filesystem::temp_directory_path(),e);
    if(!e && target.parent_path()==parent && target.filename()==directory.filename() &&
        target.filename().string().starts_with("container-native-point-advection-"))std::filesystem::remove_all(target,e);
  }
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
  void world(openvdb::GridBase& grid,openvdb::VecType tag=openvdb::VEC_CONTRAVARIANT_RELATIVE) {
    grid.setName("velocity");grid.setGridClass(openvdb::GRID_UNKNOWN);grid.setVectorType(tag);grid.setIsInWorldSpace(true);
  }
  void field(const std::string& name,openvdb::GridBase::ConstPtr grid,const openvdb::MetaMap& metadata={}) {
    openvdb::io::File file(path(name).string());file.setCompression(openvdb::io::COMPRESS_ZIP);file.write({grid},metadata);file.close();
  }
  auto run(const std::string& input="source.vdb",const std::string& output="out.vdb") {
    return vdb::WriteAdvectedNativePoints(path(input),path("velocity.vdb"),path(output),o,{},{},p,limits);
  }
  std::filesystem::path directory;vdb::NativePointAdvectionOptions o;vdb::ProcessingOptions p;vdb::PointAttributeOptions limits;
};

TEST_F(OpenVdbNativePointAdvection, SelectedConstantFlowPreservesOpaquePayloadsInactiveRecordsAndMetadata) {
  auto velocity=openvdb::Vec3DGrid::create(V(.5,-.25,0));world(*velocity);field("velocity.vdb",velocity);
  const auto original=bytes(path("source.vdb"));const auto flowBytes=bytes(path("velocity.vdb"));
  const auto before=oracle(read("source.vdb"));o.timeStep=2;o.steps=4;o.groups.anyOf={"A"};
  const auto result=run();EXPECT_FALSE(result.motion.noOp);EXPECT_EQ(result.motion.selectedCount,3);
  EXPECT_EQ(result.motion.inputCount,6);EXPECT_EQ(result.motion.outputCount,6);EXPECT_EQ(result.executedSteps,4);
  EXPECT_EQ(result.velocitySampleCount,24);EXPECT_EQ(result.motion.inputActiveCount,4);EXPECT_EQ(result.motion.outputActiveCount,4);
  EXPECT_NEAR(result.maximumTravelVelocityVoxels,std::hypot(1.,.5),1e-6);
  auto output=read("out.vdb","moved");const auto after=oracle(output);comparePayload(before,after);
  for(const auto& [id,a]:before) {
    const auto& b=after.at(id);const V expected=a.world+(a.a?V(1,-.5,0):V(0));
    EXPECT_NEAR(length(b.world-expected),0,2e-6);EXPECT_EQ(a.active,b.active);
  }
  EXPECT_EQ((*output)["artist"]->str(),std::string("raw\0artist",10));
  openvdb::io::File file(path("out.vdb").string());file.open(false);
  EXPECT_EQ((*file.getMetadata())["file:binary"]->str(),std::string("a\0b",3));file.close();
  EXPECT_EQ(original,bytes(path("source.vdb")));EXPECT_EQ(flowBytes,bytes(path("velocity.vdb")));
}

TEST_F(OpenVdbNativePointAdvection, IndependentAffineLinearFieldHasAnalyticRk2AndRk4Oracles) {
  auto source=create({{.25,0,0}});write("single.vdb",source);
  auto velocity=openvdb::Vec3DGrid::create(V(0));world(*velocity);
  auto transform=openvdb::math::Transform::createLinearTransform(.5);transform->postTranslate(V(-1,.5,-.25));
  velocity->setTransform(transform);
  for(int x=-2;x<=14;++x)for(int y=-2;y<=2;++y)for(int z=-2;z<=2;++z) {
    const C c(x,y,z);const double wx=transform->indexToWorld(c).x();
    velocity->tree().setValue(c,V(.25*wx,0,0));
  }
  field("velocity.vdb",velocity);
  for(const auto integration:{vdb::NativePointIntegration::RK2,vdb::NativePointIntegration::RK4}) {
    o.integration=integration;o.steps=4;const double a=.25/o.steps;
    const double factor=integration==vdb::NativePointIntegration::RK2?1+a+a*a/2:1+a+a*a/2+a*a*a/6+a*a*a*a/24;
    const auto output=integration==vdb::NativePointIntegration::RK2?"rk2.vdb":"rk4.vdb";
    const auto result=run("single.vdb",output);const auto after=oracle(read(output,"moved"));
    EXPECT_NEAR(after.begin()->second.world.x(),.25*std::pow(factor,o.steps),3e-8);
    EXPECT_EQ(result.velocitySampleCount,o.steps*(integration==vdb::NativePointIntegration::RK2?2:4));
  }
}

TEST_F(OpenVdbNativePointAdvection, RotationalRk4ConvergesWithSubsteps) {
  write("single.vdb",create({{1,0,0}}));
  auto velocity=openvdb::Vec3SGrid::create(openvdb::Vec3f(0));world(*velocity);
  for(int x=-3;x<=3;++x)for(int y=-3;y<=3;++y)for(int z=-3;z<=3;++z)
    velocity->tree().setValue({x,y,z},openvdb::Vec3f(float(-y),float(x),0));
  field("velocity.vdb",velocity);o.timeStep=.5;o.integration=vdb::NativePointIntegration::RK4;
  std::array<double,2> errors{};
  for(size_t n=0;n!=2;++n) {
    o.steps=n?4:1;const auto output=n?"fine.vdb":"coarse.vdb";(void)run("single.vdb",output);
    const V actual=oracle(read(output,"moved")).begin()->second.world;
    errors[n]=std::hypot(actual.x()-std::cos(.5),actual.y()-std::sin(.5));
  }
  EXPECT_LT(errors[1],errors[0]/100);EXPECT_LT(errors[1],2e-6);
}

TEST_F(OpenVdbNativePointAdvection, AllWorldTagsUseRawPhysicalComponentsForBothNativeTypes) {
  write("single.vdb",create({{0,0,0}}));o.timeStep=2;
  int ordinal=0;
  for(const auto tag:{openvdb::VEC_INVARIANT,openvdb::VEC_COVARIANT,openvdb::VEC_COVARIANT_NORMALIZE,
                      openvdb::VEC_CONTRAVARIANT_RELATIVE,openvdb::VEC_CONTRAVARIANT_ABSOLUTE}) {
    for(const bool narrow:{false,true}) {
      auto transform=openvdb::math::Transform::createLinearTransform(2);transform->postScale(V(-1,1,1));transform->postTranslate(V(10,2,-3));
      openvdb::GridBase::Ptr velocity=narrow?openvdb::GridBase::Ptr(openvdb::Vec3SGrid::create(openvdb::Vec3f(.25,.5,-.125))):
          openvdb::GridBase::Ptr(openvdb::Vec3DGrid::create(V(.25,.5,-.125)));
      world(*velocity,tag);velocity->setTransform(transform);field("velocity.vdb",velocity);
      const auto output="tag"+std::to_string(ordinal++)+".vdb";const auto result=run("single.vdb",output);
      EXPECT_EQ(oracle(read(output,"moved")).begin()->second.world,V(.5,1,-.25));
      EXPECT_NE(result.velocitySemantics.find("WORLD XYZ"),std::string::npos);
    }
  }
}

TEST_F(OpenVdbNativePointAdvection, NegativeFaceMacUsesIndependentReflectedRotatedMapAndWorldUnits) {
  auto source=create({{0,0,0}});auto pointMap=openvdb::math::Transform::createLinearTransform(1);pointMap->postTranslate(V(1,-2,3));
  source->setTransform(pointMap);write("single.vdb",source);
  openvdb::math::Mat4d m(0,2,0,0, -2,0,0,0, 0,0,-2,0, 1,-2,3,1);
  auto transform=openvdb::math::Transform::createLinearTransform(m);
  auto velocity=openvdb::Vec3DGrid::create(V(0));velocity->setName("velocity");velocity->setTransform(transform);
  velocity->setGridClass(openvdb::GRID_STAGGERED);velocity->setIsInWorldSpace(false);velocity->setVectorType(openvdb::VEC_CONTRAVARIANT_RELATIVE);
  velocity->insertMeta("container:mac_layout",openvdb::StringMetadata("negative_faces"));
  velocity->insertMeta("container:velocity_units",openvdb::StringMetadata("voxels_per_second"));
  for(int x=-4;x<=4;++x)for(int y=-4;y<=4;++y)for(int z=-4;z<=4;++z)
    velocity->tree().setValue({x,y,z},V(.25*(x-.5)+.5,.5*(y-.5)-.25,.125*(z-.5)+.25));
  field("velocity.vdb",velocity);o.timeStep=.5;o.velocityLayout=vdb::NativePointVelocityLayout::NativeMacIndex;
  const auto result=run("single.vdb");const V actual=oracle(read("out.vdb","moved")).begin()->second.world;
  const V q(.265625,-.140625,.12890625);const V expected(1-2*q.y(),-2+2*q.x(),3-2*q.z());
  EXPECT_NEAR(length(actual-expected),0,2e-7);EXPECT_EQ(result.velocitySampleCount,2);
  EXPECT_NE(result.velocitySemantics.find("negative-face"),std::string::npos);
}

TEST_F(OpenVdbNativePointAdvection, SignedTimeZeroTimeEmptySelectionAndArchivesFollowNativeMotionSemantics) {
  write("fixed.vdb",create<points::FixedPointCodec<true>>());o.timeStep=0;
  const auto unchanged=run("fixed.vdb","unchanged.vdb");EXPECT_TRUE(unchanged.motion.noOp);EXPECT_EQ(unchanged.velocitySampleCount,0);EXPECT_TRUE(unchanged.integrationSkipped);
  auto a=read("fixed.vdb"),b=read("unchanged.vdb","moved");
  for(auto leaf=a->tree().cbeginLeaf();leaf;++leaf)
    EXPECT_EQ(leaf->constAttributeArray("P"),b->tree().probeConstLeaf(leaf->origin())->constAttributeArray("P"));
  o.timeStep=-1;o.steps=2;(void)run("source.vdb","negative.vdb");
  const auto before=oracle(read("source.vdb")),after=oracle(read("negative.vdb","moved"));comparePayload(before,after);
  for(const auto& [id,v]:before)EXPECT_NEAR(after.at(id).world.x(),v.world.x()-.5,2e-6);
  o.groups.anyOf={"empty"};EXPECT_TRUE(run("source.vdb","none.vdb").motion.noOp);
  vdb::PointGroupSelection none;none.anyOf={"empty"};
  const auto archive=vdb::WriteSelectedPoints(path("source.vdb"),path("archive.vdb"),none,vdb::PointSelectionMode::KeepSelected,{},p,limits);
  ASSERT_TRUE(archive.archivedSchema);o.groups={};
  const auto copied=run("archive.vdb","archive-copy.vdb");EXPECT_TRUE(copied.motion.noOp);EXPECT_TRUE(copied.motion.archivedSchema);
  EXPECT_EQ(copied.motion.schema.groups,archive.schema.groups);EXPECT_EQ(copied.motion.outputCount,0);
}

TEST_F(OpenVdbNativePointAdvection, ReflectedTargetAndZeroTimeTargetChangeAreExplicit) {
  write("single.vdb",create({{.25,0,0}}));
  o.targetIndexToWorld=std::array<double,16>{-.5,0,0,4, 0,.5,0,0, 0,0,.5,0, 0,0,0,1};
  const auto result=run("single.vdb");EXPECT_FALSE(result.motion.noOp);
  auto output=read("out.vdb","moved");EXPECT_EQ(oracle(output).begin()->second.world,V(.75,0,0));
  EXPECT_EQ(output->transform().indexToWorld(V(0)),V(4,0,0));
  EXPECT_EQ(output->transform().baseMap()->applyJacobian(V(1,0,0)),V(-.5,0,0));
  o.timeStep=0;const auto remap=run("single.vdb","remap.vdb");EXPECT_FALSE(remap.motion.noOp);EXPECT_EQ(remap.velocitySampleCount,0);EXPECT_TRUE(remap.integrationSkipped);
  EXPECT_EQ(oracle(read("remap.vdb","moved")).begin()->second.world,V(.25,0,0));
}

TEST_F(OpenVdbNativePointAdvection, ActivityCollisionRejectsUnlessExplicitOrPolicyIsSelected) {
  auto source=create({{0,0,0},{8,0,0}});source->tree().setActiveState({0,0,0},false);write("collision.vdb",source);
  auto velocity=openvdb::Vec3DGrid::create(V(8,0,0));world(*velocity);field("velocity.vdb",velocity);o.groups.anyOf={"A"};
  EXPECT_THROW(run("collision.vdb"),std::invalid_argument);EXPECT_FALSE(std::filesystem::exists(path("out.vdb")));
  o.rejectActivityMerges=false;const auto result=run("collision.vdb");EXPECT_EQ(result.motion.inactiveRecordsActivated,1);
  EXPECT_EQ(result.motion.outputActiveCount,2);const auto after=oracle(read("out.vdb","moved"));comparePayload(oracle(source),after);
  for(const auto& [id,v]:after){(void)id;EXPECT_EQ(v.world,V(8,0,0));EXPECT_TRUE(v.active);}
}

TEST_F(OpenVdbNativePointAdvection, FloatSubnormalVelocityIsPromotedBeforeIntegrationAndFinalCodecEncoding) {
  write("single.vdb",create({{0,0,0}}));const float speed=std::numeric_limits<float>::denorm_min();
  auto velocity=openvdb::Vec3SGrid::create(openvdb::Vec3f(speed,0,0));world(*velocity);field("velocity.vdb",velocity);
  o.targetIndexToWorld=std::array<double,16>{1e-6,0,0,0, 0,1e-6,0,0, 0,0,1e-6,0, 0,0,0,1};
  const auto result=run("single.vdb");const double actual=oracle(read("out.vdb","moved")).begin()->second.world.x();
  EXPECT_GT(actual,0);EXPECT_NEAR(actual,double(speed),double(speed)*1e-5);EXPECT_EQ(result.velocitySampleCount,2);
}

TEST_F(OpenVdbNativePointAdvection, DetectableInterpolationUnderflowAndLostNegativeFractionsReject) {
  write("single.vdb",create({{.75,0,0}}));
  auto velocity=openvdb::Vec3DGrid::create(V(0));world(*velocity);
  velocity->tree().setValue({0,0,0},V(std::numeric_limits<double>::denorm_min(),0,0));field("velocity.vdb",velocity);
  try{(void)run("single.vdb");FAIL();}catch(const std::invalid_argument& e){EXPECT_NE(std::string(e.what()).find("underflow"),std::string::npos);}
  EXPECT_FALSE(std::filesystem::exists(path("out.vdb")));
  write("negative.vdb",create({{-double(std::numeric_limits<float>::denorm_min()),0,0}}));
  velocity->tree().setValue({-1,0,0},V(1,0,0));field("velocity.vdb",velocity);
  try{(void)run("negative.vdb");FAIL();}catch(const std::invalid_argument& e){EXPECT_NE(std::string(e.what()).find("complement"),std::string::npos);}
}

TEST_F(OpenVdbNativePointAdvection, RemotePointIndicesWorkAndRemoteVelocitySamplingPrecisionRejects) {
  auto source=create({{1e9+.125,0,0}});write("remote.vdb",source);(void)run("remote.vdb");
  EXPECT_NEAR(oracle(read("out.vdb","moved")).begin()->second.world.x(),1e9+.625,1e-4);
  write("single.vdb",create({{.5,0,0}}));auto velocity=openvdb::Vec3DGrid::create(V(0));world(*velocity);
  auto transform=openvdb::math::Transform::createLinearTransform(1e6);transform->postTranslate(V(1e15,0,0));velocity->setTransform(transform);
  velocity->tree().setValue({-1000000000,0,0},V(0));velocity->tree().setValue({-999999999,0,0},V(1e12,0,0));
  field("velocity.vdb",velocity);o.timeStep=1e-9;
  try{(void)run("single.vdb","unsafe.vdb");FAIL();}catch(const std::invalid_argument& e){EXPECT_NE(std::string(e.what()).find("precision"),std::string::npos);}
  EXPECT_FALSE(std::filesystem::exists(path("unsafe.vdb")));
}

TEST_F(OpenVdbNativePointAdvection, InvalidVelocityMetadataNonfiniteInactiveValuesAndModeMismatchesReject) {
  for(int which=0;which!=8;++which) {
    auto velocity=openvdb::Vec3DGrid::create(V(.5,0,0));world(*velocity);
    if(which==0)velocity->setIsInWorldSpace(false);
    if(which==1)velocity->setGridClass(openvdb::GRID_STAGGERED);
    if(which==2){velocity->removeMeta(openvdb::GridBase::META_IS_LOCAL_SPACE);velocity->insertMeta(openvdb::GridBase::META_IS_LOCAL_SPACE,openvdb::StringMetadata("false"));}
    if(which==3)velocity->tree().setValueOff({4,0,0},V(NAN,0,0));
    if(which==4)o.velocityLayout=vdb::NativePointVelocityLayout::NativeMacIndex;
    if(which==5)velocity->setGridClass(openvdb::GRID_FOG_VOLUME);
    if(which==6){velocity->removeMeta(openvdb::GridBase::META_GRID_CLASS);velocity->insertMeta(openvdb::GridBase::META_GRID_CLASS,openvdb::StringMetadata("invented-class"));}
    if(which==7){velocity->removeMeta(openvdb::GridBase::META_VECTOR_TYPE);velocity->insertMeta(openvdb::GridBase::META_VECTOR_TYPE,openvdb::StringMetadata("invented-vector"));}
    field("velocity.vdb",velocity);EXPECT_THROW(run(),std::exception)<<which;EXPECT_FALSE(std::filesystem::exists(path("out.vdb")));
    o.velocityLayout=vdb::NativePointVelocityLayout::Auto;
  }
}

TEST_F(OpenVdbNativePointAdvection, AggregateTopologyMetadataWorkAndTravelCapsRejectBeforePublication) {
  write("single.vdb",create({{0,0,0}}));auto velocity=openvdb::Vec3DGrid::create(V(0));world(*velocity);
  velocity->tree().setValue({0,0,0},V(.5,0,0));field("velocity.vdb",velocity);
  p.maxVoxelCount=512;EXPECT_THROW(run("single.vdb"),std::invalid_argument);p.maxVoxelCount=4'000'000;
  auto empty=Grid::create();empty->setName("points");write("empty.vdb",empty);o.maxWork=512;
  try{(void)run("empty.vdb");FAIL();}catch(const std::invalid_argument& e){EXPECT_NE(std::string(e.what()).find("maxWork"),std::string::npos);}
  o.maxWork=200'000'000;o.maxGrids=1;EXPECT_THROW(run("single.vdb"),std::invalid_argument);o.maxGrids=256;
  openvdb::MetaMap metadata;metadata.insertMeta("large:file",openvdb::StringMetadata(std::string(600'000,'x')));field("velocity.vdb",velocity,metadata);
  o.maxMetadataBytes=800'000;EXPECT_THROW(run("single.vdb"),std::invalid_argument);o.maxMetadataBytes=1024*1024;
  field("velocity.vdb",velocity);o.maxStepTravelVelocityVoxels=.1;EXPECT_THROW(run("single.vdb"),std::invalid_argument);
  o.maxStepTravelVelocityVoxels=16;o.maxTravelVelocityVoxels=.1;EXPECT_THROW(run("single.vdb"),std::invalid_argument);
  o.maxTravelVelocityVoxels=256;p.maxInputBytes=1024;EXPECT_THROW(run("single.vdb"),std::invalid_argument);
  EXPECT_FALSE(std::filesystem::exists(path("out.vdb")));
}

TEST_F(OpenVdbNativePointAdvection, UnicodeSnapshotsExclusivePublicationAndBothInputsRemainUnchanged) {
  const auto unicode=directory/std::filesystem::path(std::u8string(u8"流れ-é"));std::filesystem::create_directory(unicode);
  const auto source=unicode/std::filesystem::path(std::u8string(u8"入力.vdb"));
  const auto flow=unicode/std::filesystem::path(std::u8string(u8"速度.vdb"));
  const auto output=unicode/std::filesystem::path(std::u8string(u8"結果.vdb"));
  std::filesystem::copy_file(path("source.vdb"),source);std::filesystem::copy_file(path("velocity.vdb"),flow);
  const auto sourceBytes=bytes(source),flowBytes=bytes(flow);
  const auto result=vdb::WriteAdvectedNativePoints(source,flow,output,o,{},{},p,limits);EXPECT_EQ(result.motion.outputCount,6);
  const auto completed=bytes(output);EXPECT_THROW(vdb::WriteAdvectedNativePoints(source,flow,output,o,{},{},p,limits),std::invalid_argument);
  EXPECT_EQ(completed,bytes(output));EXPECT_THROW(vdb::WriteAdvectedNativePoints(source,flow,source,o,{},{},p,limits),std::invalid_argument);
  EXPECT_EQ(sourceBytes,bytes(source));EXPECT_EQ(flowBytes,bytes(flow));
}

TEST_F(OpenVdbNativePointAdvection, NonlinearSelectedFlowPreservesOpaqueStorageAndInactivePoints) {
  auto velocity=openvdb::Vec3DGrid::create(V(0));world(*velocity);
  for(int x=-20;x<=36;++x)for(int y=-1;y<=1;++y)for(int z=-1;z<=1;++z)
    velocity->tree().setValue({x,y,z},V(.0005*x*x,0,0));
  field("velocity.vdb",velocity);o.timeStep=.1;o.steps=4;o.integration=vdb::NativePointIntegration::RK4;o.groups.allOf={"A"};
  const auto before=oracle(read("source.vdb"));const auto result=run();
  const auto after=oracle(read("out.vdb","moved"));comparePayload(before,after);EXPECT_FALSE(result.integrationSkipped);
  for(const auto& [id,v]:before) {
    const auto& changed=after.at(id);EXPECT_EQ(changed.active,v.active);
    if(!v.a)EXPECT_EQ(changed.world,v.world);
    else EXPECT_GT(changed.world.x(),v.world.x());
  }
}

TEST_F(OpenVdbNativePointAdvection, FixedPointCodecTrialsPreserveStorageAndRequireExplicitCoarseTolerance) {
  auto source=create<points::FixedPointCodec<true>>();write("fixed.vdb",source);o.timeStep=.2469;
  EXPECT_THROW(run("fixed.vdb","bad.vdb"),std::invalid_argument);EXPECT_FALSE(std::filesystem::exists(path("bad.vdb")));
  o.maxPositionErrorVoxels=.01;const auto result=run("fixed.vdb");EXPECT_GT(result.motion.maxPositionErrorVoxels,1e-4);
  auto output=read("out.vdb","moved");comparePayload(oracle(source),oracle(output));
  for(auto leaf=output->tree().cbeginLeaf();leaf;++leaf)EXPECT_EQ(leaf->constAttributeArray("P").type().second,"fxpt8");
}

TEST_F(OpenVdbNativePointAdvection, RetainedVelocitySourceCloneAndTargetGrowthShareTopologyCap) {
  auto source=create({{.1,0,0},{.2,0,0}});write("single-leaf.vdb",source);
  auto velocity=openvdb::Vec3DGrid::create(V(8,0,0));world(*velocity);velocity->tree().setValue({0,0,0},V(8,0,0));field("velocity.vdb",velocity);
  o.groups.anyOf={"A"};p.maxVoxelCount=1024;
  try{(void)run("single-leaf.vdb");FAIL();}catch(const std::invalid_argument& e){EXPECT_NE(std::string(e.what()).find("joint"),std::string::npos);}
  EXPECT_FALSE(std::filesystem::exists(path("out.vdb")));
  p.maxVoxelCount=2560;const auto result=run("single-leaf.vdb");EXPECT_EQ(result.motion.outputCount,2);
  EXPECT_FALSE(result.motion.noOp);EXPECT_EQ(read("out.vdb","moved")->tree().leafCount(),2);
}

TEST_F(OpenVdbNativePointAdvection, OversizedUniformStrideRejectsBeforeFullMemoryEstimateEvenAtZeroTime) {
  auto source=create({{0,0,0},{1,0,0}});
  // Zero default skips PointAttribute's writable-handle collapse path, so the
  // fixture stores one native uniform element and never expands this stride.
  points::appendAttribute<int32_t>(source->tree(),"oversized_uniform",0,UINT32_MAX);
  ASSERT_TRUE(source->tree().cbeginLeaf()->constAttributeArray("oversized_uniform").isUniform());
  write("oversized.vdb",source);o.timeStep=0;
  try{(void)run("oversized.vdb");FAIL();}catch(const std::invalid_argument& e){EXPECT_NE(std::string(e.what()).find("size/stride"),std::string::npos);}
  EXPECT_FALSE(std::filesystem::exists(path("out.vdb")));
}

TEST_F(OpenVdbNativePointAdvection, EncodedSnapshotWorkRejectsBeforeCopyOrPaging) {
  o.maxWork=1;
  try{(void)run();FAIL();}catch(const std::invalid_argument& e){EXPECT_NE(std::string(e.what()).find("before copy"),std::string::npos);}
  EXPECT_FALSE(std::filesystem::exists(path("out.vdb")));
}

TEST_F(OpenVdbNativePointAdvection, FineVelocityPrecisionDoesNotTightenUnselectedRemotePointReconstruction) {
  write("remote.vdb",create({{0,0,0},{1e9,0,0}}));
  auto velocity=openvdb::Vec3DGrid::create(V(1e-6,0,0));world(*velocity);
  std::stringstream record(std::ios::in|std::ios::out|std::ios::binary);
  openvdb::writeString(record,openvdb::math::UniformScaleMap::mapType());
  for(double value:{1e-6,1e-6,1e6,1e12,.5e6})V(value).write(record);
  record.seekg(0);openvdb::io::setCurrentVersion(record);
  auto transform=std::make_shared<openvdb::math::Transform>();transform->read(record);velocity->setTransform(transform);
  // Preserve the explicitly serialized native tiny map during fixture writing.
  class ExactVelocity:public openvdb::Vec3DGrid {
  public:
    explicit ExactVelocity(const openvdb::Vec3DGrid& grid):openvdb::Vec3DGrid(openvdb::ConstPtrCast<TreeType>(grid.constTreePtr())) {
      insertMeta(grid);setTransform(std::make_shared<openvdb::math::Transform>(grid.transform()));
    }
    openvdb::GridBase::ConstPtr copyGrid()const override {
      return copyReplacingMetadataAndTransform(*this,std::make_shared<openvdb::math::Transform>(this->transform()));
    }
    openvdb::GridBase::Ptr copyGrid()override {
      return openvdb::ConstPtrCast<openvdb::GridBase>(static_cast<const ExactVelocity&>(*this).copyGrid());
    }
  };
  field("velocity.vdb",std::make_shared<ExactVelocity>(*velocity));o.groups.anyOf={"A"};
  const auto result=run("remote.vdb");EXPECT_EQ(result.motion.selectedCount,1);
  const auto before=oracle(read("remote.vdb")),after=oracle(read("out.vdb","moved"));comparePayload(before,after);
  for(const auto& [id,value]:before) {
    if(value.a)EXPECT_NEAR(after.at(id).world.x(),1e-6,1e-12);
    else EXPECT_EQ(after.at(id).world,value.world);
  }
  EXPECT_EQ(result.motion.maxUnselectedPositionErrorVoxels,0);
}

#endif
} // namespace

