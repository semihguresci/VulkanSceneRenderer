#include "Container/geometry/OpenVdbFracture.h"
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>
#include <limits>
#include <numbers>
#include <sstream>
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
#include <openvdb/openvdb.h>
#include <openvdb/io/File.h>
#include <openvdb/tools/LevelSetSphere.h>
#include <openvdb/tools/Composite.h>
#endif
namespace {
namespace vdb=container::geometry::vdb;
TEST(OpenVdbFractureAvailability, DisabledPrecedesPathsAndOptions) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  GTEST_SKIP();
#else
  vdb::FractureOptions o;o.maxPieces=0;
  try {(void)vdb::WriteFracturedLevelSet("missing","missing",{},o,{}, {},{.maxInputBytes=0});FAIL();}
  catch(const std::runtime_error &e) {EXPECT_NE(std::string(e.what()).find("ENABLE_OPENVDB=ON"),std::string::npos);}
#endif
}
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
class OpenVdbFracture:public ::testing::Test {
protected:
  std::filesystem::path directory;vdb::ProcessingOptions p;
  void SetUp() override {openvdb::initialize();static std::atomic_uint64_t serial{};directory=std::filesystem::temp_directory_path()/("container-fracture-tests-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+"-"+std::to_string(++serial));ASSERT_TRUE(std::filesystem::create_directory(directory));p.gridName="pieces";p.maxInputBytes=2ull*1024*1024*1024;}
  void TearDown() override {std::error_code ec;std::filesystem::remove_all(directory,ec);}
  auto path(const std::string &s) {return directory/s;}
  void write(const std::string &name,const openvdb::GridPtrVec &grids,bool stats=true) {
    openvdb::MetaMap m;m.insertMeta("scene",openvdb::StringMetadata("retained"));openvdb::io::File file(path(name).string());file.setCompression(openvdb::io::COMPRESS_ZIP);file.setGridStatsMetadataEnabled(stats);file.write(grids,m);file.close();
  }
  template<class Grid> auto sphere(double radius=1,openvdb::Vec3d center=openvdb::Vec3d(0)) {
    auto g=openvdb::tools::createLevelSetSphere<Grid>(radius,center,.25,3);g->setName("source");g->insertMeta("tag",openvdb::StringMetadata("source preserved"));return g;
  }
  template<class Grid> auto read(const std::string &file,const std::string &name) {
    openvdb::io::File f(path(file).string());f.open(false);EXPECT_EQ(f.getMetadata()->metaValue<std::string>("scene"),"retained");auto g=openvdb::gridPtrCast<Grid>(f.readGrid(name));f.close();return g;
  }
  std::string bytes(const std::string &file) {std::ifstream f(path(file),std::ios::binary);return {std::istreambuf_iterator<char>(f),{}};}
  void failure(const vdb::FractureOptions &o={},const vdb::ProcessingOptions *limits=nullptr) {
    static uint32_t n{};const auto name="failed-"+std::to_string(++n)+".vdb";
    EXPECT_THROW((void)vdb::WriteFracturedLevelSet(path("source.vdb"),path("cutter.vdb"),path(name),o,{}, {},limits?*limits:p),std::exception);EXPECT_FALSE(std::filesystem::exists(path(name)));
  }
};
TEST_F(OpenVdbFracture, NativeFloatIntersectionResidualPartitionSignsMetadataAndSourcePreservation) {
  auto a=sphere<openvdb::FloatGrid>(),c=sphere<openvdb::FloatGrid>(1,{.75,0,0});write("source.vdb",{a});write("cutter.vdb",{c});const auto original=bytes("source.vdb"),tool=bytes("cutter.vdb");
  const auto r=vdb::WriteFracturedLevelSet(path("source.vdb"),path("cutter.vdb"),path("out.vdb"),{}, {},{},p);ASSERT_EQ(r.pieces.size(),2u);EXPECT_TRUE(r.pieces[0].residual);EXPECT_FALSE(r.pieces[1].residual);EXPECT_EQ(r.inputNegativeVoxelCount,r.outputNegativeVoxelCount+r.cutBoundaryVoxelCount);
  auto residual=read<openvdb::FloatGrid>("out.vdb",r.pieces[0].grid.gridName),fragment=read<openvdb::FloatGrid>("out.vdb",r.pieces[1].grid.gridName);
  EXPECT_LT(residual->tree().getValue({-3,0,0}),0);EXPECT_GT(fragment->tree().getValue({-3,0,0}),0);EXPECT_LT(fragment->tree().getValue({2,0,0}),0);EXPECT_GT(residual->tree().getValue({2,0,0}),0);
  EXPECT_EQ(fragment->metaValue<std::string>("tag"),"source preserved");EXPECT_FALSE(fragment->saveFloatAsHalf());EXPECT_EQ(bytes("source.vdb"),original);EXPECT_EQ(bytes("cutter.vdb"),tool);
  EXPECT_NEAR(double(r.inputNegativeVoxelCount)*.25*.25*.25,4*std::numbers::pi/3,.5);
}
TEST_F(OpenVdbFracture, NativeDoubleFullyContainedAndDisjointCuttersHaveCorrectWholePieceRoles) {
  auto source=sphere<openvdb::DoubleGrid>();write("source.vdb",{source});auto c=sphere<openvdb::DoubleGrid>(1.5);write("cutter.vdb",{c});
  auto inside=vdb::WriteFracturedLevelSet(path("source.vdb"),path("cutter.vdb"),path("inside.vdb"),{}, {},{},p);ASSERT_EQ(inside.pieces.size(),1u);EXPECT_FALSE(inside.pieces[0].residual);EXPECT_EQ(inside.outputNegativeVoxelCount,inside.inputNegativeVoxelCount);
  c=sphere<openvdb::DoubleGrid>(1,{3,0,0});write("cutter.vdb",{c});auto outside=vdb::WriteFracturedLevelSet(path("source.vdb"),path("cutter.vdb"),path("outside.vdb"),{}, {},{},p);ASSERT_EQ(outside.pieces.size(),1u);EXPECT_TRUE(outside.pieces[0].residual);EXPECT_EQ(outside.outputNegativeVoxelCount,outside.inputNegativeVoxelCount);
}
TEST_F(OpenVdbFracture, ExactIntegerWorldTranslationsMatchDirectNativeCutterGeometry) {
  auto source=sphere<openvdb::DoubleGrid>(),c=sphere<openvdb::DoubleGrid>();write("source.vdb",{source});write("cutter.vdb",{c});vdb::FractureOptions o;o.instances.push_back({.translation={.75,0,0}});
  auto translated=vdb::WriteFracturedLevelSet(path("source.vdb"),path("cutter.vdb"),path("translated.vdb"),o,{}, {},p);
  c=sphere<openvdb::DoubleGrid>(1,{.75,0,0});write("cutter.vdb",{c});auto direct=vdb::WriteFracturedLevelSet(path("source.vdb"),path("cutter.vdb"),path("direct.vdb"),{}, {},{},p);
  ASSERT_EQ(translated.pieces.size(),direct.pieces.size());for(size_t n=0;n<direct.pieces.size();++n) {auto a=read<openvdb::DoubleGrid>("translated.vdb",translated.pieces[n].grid.gridName),b=read<openvdb::DoubleGrid>("direct.vdb",direct.pieces[n].grid.gridName);EXPECT_EQ(a->activeVoxelCount(),b->activeVoxelCount());for(auto it=a->cbeginValueOn();it;++it) EXPECT_EQ(double(*it),b->tree().getValue(it.getCoord()));}
}
TEST_F(OpenVdbFracture, RigidRotationAndMultipleInstancesHaveBoundedDeterministicPartition) {
  auto source=sphere<openvdb::FloatGrid>(1.5),c=sphere<openvdb::FloatGrid>(1,{.75,0,0});write("source.vdb",{source});write("cutter.vdb",{c});vdb::FractureOptions o;o.instances.push_back({.rotation={0,0,std::numbers::pi/2}});o.instances.push_back({.translation={-.75,0,0}});
  for(bool overlap:{false,true}) {o.cutterOverlap=overlap;const auto file=overlap?"overlap.vdb":"separate.vdb";auto r=vdb::WriteFracturedLevelSet(path("source.vdb"),path("cutter.vdb"),path(file),o,{}, {},p);EXPECT_GE(r.pieces.size(),2u);EXPECT_EQ(r.outputNegativeVoxelCount+r.cutBoundaryVoxelCount,r.inputNegativeVoxelCount);}
  auto repeat=vdb::WriteFracturedLevelSet(path("source.vdb"),path("cutter.vdb"),path("repeat.vdb"),o,{}, {},p);auto first=read<openvdb::FloatGrid>("overlap.vdb",repeat.pieces[0].grid.gridName),second=read<openvdb::FloatGrid>("repeat.vdb",repeat.pieces[0].grid.gridName);EXPECT_EQ(first->activeVoxelCount(),second->activeVoxelCount());for(auto it=first->cbeginValueOn();it;++it) EXPECT_EQ(float(*it),second->tree().getValue(it.getCoord()));
}
TEST_F(OpenVdbFracture, RepresentedSmallFragmentsDoNotUseNativeTwentySevenVoxelDiscard) {
  auto a=sphere<openvdb::DoubleGrid>(),c=sphere<openvdb::DoubleGrid>(.4,{.8,0,0});write("source.vdb",{a});write("cutter.vdb",{c});auto r=vdb::WriteFracturedLevelSet(path("source.vdb"),path("cutter.vdb"),path("small.vdb"),{}, {},{},p);ASSERT_EQ(r.pieces.size(),2u);EXPECT_GT(r.pieces[1].negativeVoxelCount,0u);EXPECT_LT(r.pieces[1].negativeVoxelCount,27u);
  auto sparse=openvdb::DoubleGrid::create(.75);sparse->setName("source");sparse->setGridClass(openvdb::GRID_LEVEL_SET);sparse->setTransform(openvdb::math::Transform::createLinearTransform(.25));
  sparse->tree().setValue({0,0,0},-.01);for(int x:{-1,1}) for(int y:{-1,1}) for(int z:{-1,1}) sparse->tree().setValue({x,y,z},.5);
  write("source.vdb",{sparse});c=sphere<openvdb::DoubleGrid>(2);write("cutter.vdb",{c});auto nine=vdb::WriteFracturedLevelSet(path("source.vdb"),path("cutter.vdb"),path("nine.vdb"),{}, {},{},p);ASSERT_EQ(nine.pieces.size(),1u);EXPECT_FALSE(nine.pieces[0].residual);EXPECT_GT(nine.pieces[0].grid.activeVoxelCount,0u);EXPECT_LT(nine.pieces[0].grid.activeVoxelCount,27u);
}
TEST_F(OpenVdbFracture, NestedCavitiesSurviveUnsegmentedCutsAndSurfaceSegmentationRejectsInvertedPieces) {
  auto outer=sphere<openvdb::FloatGrid>(1.5),inner=sphere<openvdb::FloatGrid>(.5);openvdb::tools::csgDifference(*outer,*inner);auto cutter=sphere<openvdb::FloatGrid>(1,{.75,0,0});write("source.vdb",{outer});write("cutter.vdb",{cutter});auto r=vdb::WriteFracturedLevelSet(path("source.vdb"),path("cutter.vdb"),path("cavity.vdb"),{}, {},{},p);EXPECT_EQ(r.outputNegativeVoxelCount+r.cutBoundaryVoxelCount,r.inputNegativeVoxelCount);
  for(const auto &piece:r.pieces) EXPECT_GE(read<openvdb::FloatGrid>("cavity.vdb",piece.grid.gridName)->tree().getValue({0,0,0}),0);
  // Disjoint cutter leaves the complete two-boundary cavity object to segment.
  cutter=sphere<openvdb::FloatGrid>(1,{4,0,0});write("cutter.vdb",{cutter});vdb::FractureOptions o;o.segment=true;o.maxPieces=4;
  try {(void)vdb::WriteFracturedLevelSet(path("source.vdb"),path("cutter.vdb"),path("segmented-cavity.vdb"),o,{}, {},p);FAIL();}
  catch(const std::invalid_argument &error) {const std::string message=error.what();EXPECT_TRUE(message.find("negative exterior")!=std::string::npos || message.find("negative-exterior")!=std::string::npos)<<message;}
  EXPECT_FALSE(std::filesystem::exists(path("segmented-cavity.vdb")));
}
TEST_F(OpenVdbFracture, NativeSurfaceSegmentationSplitsDisconnectedPositiveExteriorSolids) {
  auto a=sphere<openvdb::DoubleGrid>(1,{-2,0,0}),b=sphere<openvdb::DoubleGrid>(1,{2,0,0});openvdb::tools::csgUnion(*a,*b);auto c=sphere<openvdb::DoubleGrid>(.5);
  write("source.vdb",{a});write("cutter.vdb",{c});vdb::FractureOptions o;o.segment=true;o.maxPieces=4;
  const auto r=vdb::WriteFracturedLevelSet(path("source.vdb"),path("cutter.vdb"),path("segmented.vdb"),o,{}, {},p);ASSERT_EQ(r.pieces.size(),2u);EXPECT_TRUE(r.pieces[0].residual);EXPECT_TRUE(r.pieces[1].residual);EXPECT_EQ(r.outputNegativeVoxelCount+r.cutBoundaryVoxelCount,r.inputNegativeVoxelCount);
  auto first=read<openvdb::DoubleGrid>("segmented.vdb",r.pieces[0].grid.gridName),second=read<openvdb::DoubleGrid>("segmented.vdb",r.pieces[1].grid.gridName);
  EXPECT_LT(first->tree().getValue({-8,0,0}),0);EXPECT_GT(first->tree().getValue({8,0,0}),0);EXPECT_GT(second->tree().getValue({-8,0,0}),0);EXPECT_LT(second->tree().getValue({8,0,0}),0);
}
TEST_F(OpenVdbFracture, WorldRotationPlacesOffsetCutterAndRespectsSourceMapOrientation) {
  auto a=sphere<openvdb::DoubleGrid>(1.5),c=sphere<openvdb::DoubleGrid>(1,{.75,0,0});write("source.vdb",{a});write("cutter.vdb",{c});vdb::FractureOptions o;o.instances.push_back({.rotation={0,0,std::numbers::pi/2}});
  const auto r=vdb::WriteFracturedLevelSet(path("source.vdb"),path("cutter.vdb"),path("rotated.vdb"),o,{}, {},p);ASSERT_EQ(r.pieces.size(),2u);auto f=read<openvdb::DoubleGrid>("rotated.vdb",r.pieces[1].grid.gridName);EXPECT_LT(f->tree().getValue({0,4,0}),0);EXPECT_GT(f->tree().getValue({0,-4,0}),0);
  a->transform().postRotate(std::numbers::pi/2,openvdb::math::Z_AXIS);c->setTransform(std::make_shared<openvdb::math::Transform>(a->transform()));write("source.vdb",{a});write("cutter.vdb",{c});o.instances={vdb::FractureInstance{.translation={.75,0,0}}};
  const auto oriented=vdb::WriteFracturedLevelSet(path("source.vdb"),path("cutter.vdb"),path("oriented.vdb"),o,{}, {},p);auto shifted=read<openvdb::DoubleGrid>("oriented.vdb",oriented.pieces.back().grid.gridName);EXPECT_LT(shifted->tree().getValue({3,-3,0}),0);EXPECT_GT(shifted->tree().getValue({3,4,0}),0);
}
TEST_F(OpenVdbFracture, PersistedSmallScaleAndDoubleTranslationAvoidAbsoluteDeterminantAndFloatParameters) {
  auto a=sphere<openvdb::DoubleGrid>(),c=sphere<openvdb::DoubleGrid>();
  for(const auto &g:{a,c}) {
    for(auto value=g->beginValueAll();value;++value) value.setValue(double(*value)*4e-6);g->tree().root().setBackground(3e-6,false);
    std::stringstream record(std::ios::in|std::ios::out|std::ios::binary);openvdb::writeString(record,openvdb::math::UniformScaleMap::mapType());for(double n:{1e-6,1e-6,1e6,1e12,5e5}) openvdb::Vec3d(n).write(record);record.seekg(0);openvdb::io::setCurrentVersion(record);g->transform().read(record);
  }
  write("source.vdb",{a});write("cutter.vdb",{c});vdb::FractureOptions o;o.instances.push_back({.translation={3e-6,0,0}});const auto r=vdb::WriteFracturedLevelSet(path("source.vdb"),path("cutter.vdb"),path("small-map.vdb"),o,{}, {},p);ASSERT_EQ(r.pieces.size(),2u);EXPECT_EQ(r.pieces[0].grid.voxelSize,1e-6);
  auto f=read<openvdb::DoubleGrid>("small-map.vdb",r.pieces[1].grid.gridName);EXPECT_LT(f->tree().getValue({2,0,0}),0);EXPECT_GT(f->tree().getValue({-3,0,0}),0);EXPECT_EQ(f->transform().baseMap()->applyJacobian(openvdb::Vec3d(1,0,0)),openvdb::Vec3d(1e-6,0,0));
}
TEST_F(OpenVdbFracture, PreflightOptionsPrecisionMapClosureAndAggregateLimitsRejectWithoutOutput) {
  auto a=sphere<openvdb::FloatGrid>(),c=sphere<openvdb::FloatGrid>(1,{.75,0,0});write("source.vdb",{a});write("cutter.vdb",{c});vdb::FractureOptions o;o.maxPieces=1;failure(o);o={};o.maxWorkItems=10;failure(o);o={};o.instances.resize(33);failure(o);o={};o.instances.push_back({.translation={1e100,0,0}});failure(o);
  auto limits=p;limits.maxVoxelCount=200;failure({},&limits);limits=p;limits.maxInputBytes=1024;failure({},&limits);
  c->transform().postTranslate(openvdb::Vec3d(1e-12,0,0));write("cutter.vdb",{c});failure();auto d=sphere<openvdb::DoubleGrid>();write("cutter.vdb",{d});failure();
  c=sphere<openvdb::FloatGrid>();c->tree().setValueOff({100,100,100},-.75f);write("cutter.vdb",{c});failure();write("cutter.vdb",{a},false);failure();
}
TEST_F(OpenVdbFracture, ExactRemoteMapsUnicodeParentsAndExclusiveOutputSurvive) {
  auto a=sphere<openvdb::DoubleGrid>(),c=sphere<openvdb::DoubleGrid>(1,{.75,0,0});
  for(const auto &g:{a,c}) {for(auto value=g->beginValueAll();value;++value) value.setValue(double(*value)*.4);g->tree().root().setBackground(.75*.4,false);g->setTransform(openvdb::math::Transform::createLinearTransform(.1));}
  a->transform().postTranslate(openvdb::Vec3d(1e8,-2e8,3e8));c->setTransform(std::make_shared<openvdb::math::Transform>(a->transform()));write("source.vdb",{a});write("cutter.vdb",{c});auto parent=directory/std::filesystem::path(L"\u7834\u788e-\u00e9");ASSERT_TRUE(std::filesystem::create_directory(parent));auto out=parent/"pieces.vdb";
  auto r=vdb::WriteFracturedLevelSet(path("source.vdb"),path("cutter.vdb"),out,{}, {},{},p);std::filesystem::copy_file(out,path("readback.vdb"));auto g=read<openvdb::DoubleGrid>("readback.vdb",r.pieces.front().grid.gridName);EXPECT_EQ(g->transform().baseMap()->getAffineMap()->getMat4(),a->transform().baseMap()->getAffineMap()->getMat4());const auto before=std::filesystem::file_size(out);EXPECT_THROW((void)vdb::WriteFracturedLevelSet(path("source.vdb"),path("cutter.vdb"),out,{}, {},{},p),std::exception);EXPECT_EQ(std::filesystem::file_size(out),before);
}
TEST_F(OpenVdbFracture, ZeroRotationOnRotatedMapUsesExactOriginalCutterValues) {
  auto a=sphere<openvdb::DoubleGrid>(),c=sphere<openvdb::DoubleGrid>(1,{.75,0,0});a->transform().postRotate(.3,openvdb::math::Z_AXIS);a->transform().postRotate(.2,openvdb::math::X_AXIS);c->setTransform(std::make_shared<openvdb::math::Transform>(a->transform()));
  write("source.vdb",{a});write("cutter.vdb",{c});const auto r=vdb::WriteFracturedLevelSet(path("source.vdb"),path("cutter.vdb"),path("identity.vdb"),{}, {},{},p);ASSERT_EQ(r.pieces.size(),2u);
  auto fragment=read<openvdb::DoubleGrid>("identity.vdb",r.pieces[1].grid.gridName),residual=read<openvdb::DoubleGrid>("identity.vdb",r.pieces[0].grid.gridName);
  for(auto value=fragment->cbeginValueOn();value;++value) EXPECT_EQ(double(*value),std::max(a->tree().getValue(value.getCoord()),c->tree().getValue(value.getCoord())));
  for(auto value=residual->cbeginValueOn();value;++value) EXPECT_EQ(double(*value),std::max(a->tree().getValue(value.getCoord()),-c->tree().getValue(value.getCoord())));
}
#endif
} // namespace
