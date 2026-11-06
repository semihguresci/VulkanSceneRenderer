#include "Container/geometry/OpenVdbFastSweeping.h"
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>
#include <limits>
#include <cstdlib>
#include <sstream>
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
#include <openvdb/openvdb.h>
#include <openvdb/io/File.h>
#include <openvdb/tools/LevelSetSphere.h>
#endif
namespace {
namespace vdb=container::geometry::vdb;
using Op=vdb::FastSweepingOperation;
TEST(OpenVdbFastSweepingAvailability, DisabledPrecedesPathsAndOptions) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  GTEST_SKIP();
#else
  vdb::FastSweepingOptions o;o.iterations=0;
  for(bool extension:{false,true}) try {
    if(extension) (void)vdb::WriteFastExtendedField("missing","missing",{},false,o);
    else (void)vdb::WriteFastSwept("missing",{},static_cast<Op>(99),o);
    FAIL();
  } catch(const std::runtime_error &e) {EXPECT_NE(std::string(e.what()).find("ENABLE_OPENVDB=ON"),std::string::npos);}
#endif
}
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
class OpenVdbFastSweeping:public ::testing::Test {
protected:
  std::filesystem::path directory;vdb::ProcessingOptions p;
  void SetUp() override {
    openvdb::initialize();static std::atomic_uint64_t serial{};
    directory=std::filesystem::temp_directory_path()/("container-fast-sweep-tests-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+"-"+std::to_string(++serial));
    ASSERT_TRUE(std::filesystem::create_directory(directory));p.gridName="distance";
    p.maxInputBytes=1024ull*1024*1024;
  }
  void TearDown() override {std::error_code ec;std::filesystem::remove_all(directory,ec);}
  auto path(const std::string &s) {return directory/s;}
  void write(const std::string &name,const openvdb::GridPtrVec &grids,bool stats=true) {
    openvdb::MetaMap meta;meta.insertMeta("scene",openvdb::StringMetadata("retained"));
    openvdb::io::File f(path(name).string());f.setCompression(openvdb::io::COMPRESS_ZIP);f.setGridStatsMetadataEnabled(stats);f.write(grids,meta);f.close();
  }
  template<class Grid> auto read(const std::string &name,const std::string &grid="distance") {
    openvdb::io::File f(path(name).string());f.open(false);auto result=openvdb::gridPtrCast<Grid>(f.readGrid(grid));EXPECT_EQ(f.getMetadata()->metaValue<std::string>("scene"),"retained");f.close();return result;
  }
  template<class Grid> auto plane(double h=.25,bool fog=false) {
    using V=typename Grid::ValueType;
    auto g=Grid::create(V(fog?-20:20));g->setName("source");g->setTransform(openvdb::math::Transform::createLinearTransform(h));
    g->setGridClass(fog?openvdb::GRID_FOG_VOLUME:openvdb::GRID_LEVEL_SET);
    for(int x=-5;x<=5;++x) for(int y=-3;y<=3;++y) for(int z=-3;z<=3;++z) g->tree().setValueOff({x,y,z},V((fog?-1:1)*(x-.5)*h*2));
    for(int x=-4;x<=4;++x) for(int y=-2;y<=2;++y) for(int z=-2;z<=2;++z) g->tree().setValue({x,y,z},V((fog?-1:1)*(x-.5)*h*2));
    g->insertMeta("tag",openvdb::StringMetadata("source retained"));return g;
  }
  template<class Grid> auto field(const Grid &source) {
    auto f=Grid::create(typename Grid::ValueType(11));f->setTransform(std::make_shared<openvdb::math::Transform>(source.transform()));f->setName("field");
    for(int x=-5;x<=5;++x) for(int y=-3;y<=3;++y) for(int z=-3;z<=3;++z) f->tree().setValue({x,y,z},typename Grid::ValueType(2*y+3*z+7*x));
    f->insertMeta("tag",openvdb::StringMetadata("field retained"));return f;
  }
  void fail(const std::string &input,Op op,const vdb::FastSweepingOptions &o={},const vdb::ProcessingOptions *limits=nullptr,const std::string &selector={}) {
    static uint32_t serial{};auto out=path("failed-"+std::to_string(++serial)+".vdb");
    EXPECT_THROW((void)vdb::WriteFastSwept(path(input),out,op,o,selector,limits?*limits:p),std::exception);EXPECT_FALSE(std::filesystem::exists(out));
  }
};
TEST_F(OpenVdbFastSweeping, FloatFogAndDoubleReinitializationHaveAnalyticWorldPlaneDistances) {
  auto fog=plane<openvdb::FloatGrid>(.125,true);write("fog.vdb",{fog});
  const auto rf=vdb::WriteFastSwept(path("fog.vdb"),path("fog-out.vdb"),Op::FogToSdf,{}, {},p);
  auto f=read<openvdb::FloatGrid>("fog-out.vdb");EXPECT_EQ(f->activeVoxelCount(),fog->activeVoxelCount());EXPECT_GT(rf.boundaryVoxelCount,0u);EXPECT_GT(rf.sweepingVoxelCount,0u);
  for(int x=-4;x<=4;++x) EXPECT_NEAR(f->tree().getValue({x,0,0}),(x-.5)*.125,1e-6);
  auto sdf=plane<openvdb::DoubleGrid>(.5);write("sdf.vdb",{sdf});
  (void)vdb::WriteFastSwept(path("sdf.vdb"),path("sdf-out.vdb"),Op::ReinitializeSdf,{}, {},p);
  auto d=read<openvdb::DoubleGrid>("sdf-out.vdb");for(int x=-4;x<=4;++x) EXPECT_NEAR(d->tree().getValue({x,0,0}),(x-.5)*.5,1e-12);
  EXPECT_EQ(d->metaValue<std::string>("tag"),"source retained");EXPECT_FALSE(d->saveFloatAsHalf());
}
TEST_F(OpenVdbFastSweeping, ScalarAndWorldVectorExtensionMatchIndependentTangentialFunctions) {
  auto source=plane<openvdb::DoubleGrid>();auto f=field(*source);write("sdf.vdb",{source});write("field.vdb",{f});
  const auto r=vdb::WriteFastExtendedField(path("sdf.vdb"),path("field.vdb"),path("extended.vdb"),false,{}, {},{},p);
  auto ext=read<openvdb::DoubleGrid>("extended.vdb","distance_extension");EXPECT_EQ(ext->activeVoxelCount(),source->activeVoxelCount());EXPECT_EQ(r.extensionGrid.activeVoxelCount,source->activeVoxelCount());
  for(int x=-4;x<=4;++x) for(int y=-1;y<=1;++y) for(int z=-1;z<=1;++z) EXPECT_NEAR(ext->tree().getValue({x,y,z}),2*y+3*z+3.5,1e-11);
  EXPECT_EQ(ext->metaValue<std::string>("tag"),"field retained");
  auto fog=plane<openvdb::FloatGrid>(.25,true);auto v=openvdb::Vec3SGrid::create(openvdb::Vec3f(13));v->setName("vector");v->setVectorType(openvdb::VEC_CONTRAVARIANT_RELATIVE);v->setTransform(std::make_shared<openvdb::math::Transform>(fog->transform()));
  for(int x=-5;x<=5;++x) for(int y=-3;y<=3;++y) for(int z=-3;z<=3;++z) v->tree().setValue({x,y,z},openvdb::Vec3f(float(x),float(y+2*x),float(z-3*x)));
  write("fog.vdb",{fog});write("vector.vdb",{v});(void)vdb::WriteFastExtendedField(path("fog.vdb"),path("vector.vdb"),path("vector-out.vdb"),true,{}, {},{},p);
  auto ev=read<openvdb::Vec3SGrid>("vector-out.vdb","distance_extension");EXPECT_EQ(ev->getVectorType(),v->getVectorType());
  for(int x=-4;x<=4;++x) {const auto a=ev->tree().getValue({x,0,0});EXPECT_NEAR(a.x(),.5,1e-5);EXPECT_NEAR(a.y(),1,1e-5);EXPECT_NEAR(a.z(),-1.5,1e-5);}
}
TEST_F(OpenVdbFastSweeping, OneSidedExtensionPreservesOppositeBoundaryAndComputingValues) {
  auto source=plane<openvdb::FloatGrid>();auto f=field(*source);write("sdf.vdb",{source});write("field.vdb",{f});
  for(auto side:{vdb::FastSweepingSide::GreaterThanIsovalue,vdb::FastSweepingSide::LessThanIsovalue}) {
    vdb::FastSweepingOptions o;o.side=side;const auto name=side==vdb::FastSweepingSide::GreaterThanIsovalue?"positive.vdb":"negative.vdb";
    (void)vdb::WriteFastExtendedField(path("sdf.vdb"),path("field.vdb"),path(name),false,o,{}, {},p);auto ext=read<openvdb::FloatGrid>(name,"distance_extension");
    for(int x=-4;x<=4;++x) {const bool selected=side==vdb::FastSweepingSide::GreaterThanIsovalue?x>=1:x<=0;EXPECT_NEAR(ext->tree().getValue({x,0,0}),selected?3.5:7*x,1e-5);}
  }
}
TEST_F(OpenVdbFastSweeping, ActivityMaskRestrictsDomainAndIgnoresMaskValuesAndClass) {
  auto s=plane<openvdb::DoubleGrid>();auto mask=openvdb::DoubleGrid::create(std::numeric_limits<double>::quiet_NaN());mask->setName("mask");mask->setGridClass(openvdb::GRID_LEVEL_SET);mask->setTransform(std::make_shared<openvdb::math::Transform>(s->transform()));
  for(int x=-3;x<=3;++x) for(int z=-2;z<=2;++z) mask->tree().setValue({x,0,z},std::numeric_limits<double>::quiet_NaN());
  write("sdf.vdb",{s});write("mask.vdb",{mask});vdb::FastSweepingOptions o;o.mask=path("mask.vdb");
  (void)vdb::WriteFastSwept(path("sdf.vdb"),path("restricted.vdb"),Op::ReinitializeSdf,o,{},p);auto result=read<openvdb::DoubleGrid>("restricted.vdb");EXPECT_EQ(result->activeVoxelCount(),35u);EXPECT_FALSE(result->tree().isValueOn({0,1,0}));
  EXPECT_NEAR(result->tree().getValue({-3,0,0}),-.875,1e-12);
  auto empty=openvdb::BoolGrid::create(false);empty->setName("empty");empty->setTransform(std::make_shared<openvdb::math::Transform>(s->transform()));write("empty.vdb",{empty});o.mask=path("empty.vdb");fail("sdf.vdb",Op::ReinitializeSdf,o);
}
TEST_F(OpenVdbFastSweeping, SdfBandDilationAndMaskUnionPreserveSeedsAndExtendAnalyticPlane) {
  auto s=plane<openvdb::DoubleGrid>();s->tree().clear();s->tree().root().setBackground(1.0,false);
  for(int x=-5;x<=5;++x) for(int y=-5;y<=5;++y) for(int z=-5;z<=5;++z) s->tree().setValueOff({x,y,z},x<=0?-1.0:1.0);
  for(int x=-1;x<=2;++x) for(int y=-2;y<=2;++y) for(int z=-2;z<=2;++z) s->tree().setValue({x,y,z},(x-.5)*.25);
  write("sdf.vdb",{s});vdb::FastSweepingOptions o;o.dilation=2;o.side=vdb::FastSweepingSide::GreaterThanIsovalue;
  (void)vdb::WriteFastSwept(path("sdf.vdb"),path("dilated.vdb"),Op::DilateSdf,o,{},p);auto d=read<openvdb::DoubleGrid>("dilated.vdb");EXPECT_TRUE(d->tree().isValueOn({4,0,0}));EXPECT_FALSE(d->tree().isValueOn({-3,0,0}));EXPECT_NEAR(d->tree().getValue({4,0,0}),.875,1e-12);
  for(auto it=s->cbeginValueOn();it;++it) EXPECT_EQ(d->tree().getValue(it.getCoord()),double(*it));
  auto mask=openvdb::BoolGrid::create(false);mask->setTransform(std::make_shared<openvdb::math::Transform>(s->transform()));mask->setName("mask");for(int x=-3;x<=4;++x) mask->tree().setValue({x,0,0},false);write("mask.vdb",{mask});o={};o.mask=path("mask.vdb");
  (void)vdb::WriteFastSwept(path("sdf.vdb"),path("union.vdb"),Op::MaskSdf,o,{},p);auto u=read<openvdb::DoubleGrid>("union.vdb");EXPECT_NEAR(u->tree().getValue({4,0,0}),.875,1e-12);EXPECT_NEAR(u->tree().getValue({-3,0,0}),-.875,1e-12);
}
TEST_F(OpenVdbFastSweeping, TopologyConversionUsesActivityAndWorldUnits) {
  auto mask=openvdb::BoolGrid::create(false);mask->setName("mask");mask->setTransform(openvdb::math::Transform::createLinearTransform(.5));
  for(int x=-2;x<=2;++x) for(int y=-2;y<=2;++y) for(int z=-2;z<=2;++z) mask->tree().setValue({x,y,z},false);
  write("topology.vdb",{mask});auto r=vdb::WriteFastSwept(path("topology.vdb"),path("topology-out.vdb"),Op::TopologyToSdf,{}, {},p);auto a=read<openvdb::FloatGrid>("topology-out.vdb");EXPECT_EQ(r.grid.voxelSize,.5);
  EXPECT_NEAR(a->background(),1.5,1e-6);EXPECT_LT(a->tree().getValue({0,0,0}),-1);
  // The finite PDE normalization is approximate near the six faces/corners.
  // Check the independently known half-voxel interface and symmetric signs;
  // distance error stays below 0.2 voxel rather than assuming exact redistance.
  EXPECT_NEAR(a->tree().getValue({2,0,0}),-.25,.1);EXPECT_NEAR(a->tree().getValue({3,0,0}),.25,.1);
}
TEST_F(OpenVdbFastSweeping, EmptyMissingCrossingAndDisconnectedUnreachableDomainsFail) {
  auto empty=openvdb::FloatGrid::create(1);empty->setGridClass(openvdb::GRID_LEVEL_SET);empty->setName("source");write("empty.vdb",{empty});fail("empty.vdb",Op::ReinitializeSdf);
  empty->tree().setValue({0,0,0},.5);write("no-cross.vdb",{empty});fail("no-cross.vdb",Op::ReinitializeSdf);
  auto s=plane<openvdb::FloatGrid>();s->tree().setValue({20,0,0},1);write("disconnected.vdb",{s});fail("disconnected.vdb",Op::ReinitializeSdf);
}
TEST_F(OpenVdbFastSweeping, TinyFogNormalizationAndUnresolvableContrastAreExplicit) {
  auto fog=plane<openvdb::FloatGrid>(.25,true);
  for(auto it=fog->beginValueAll();it;++it) it.setValue(float(*it)*1e-25f);fog->tree().root().setBackground(-2e-24f,false);
  write("tiny.vdb",{fog});(void)vdb::WriteFastSwept(path("tiny.vdb"),path("tiny-out.vdb"),Op::FogToSdf,{}, {},p);
  auto result=read<openvdb::FloatGrid>("tiny-out.vdb");EXPECT_NEAR(result->tree().getValue({-4,0,0}),-1.125,2e-6);EXPECT_NEAR(result->tree().getValue({4,0,0}),.875,2e-6);
  fog->tree().setValue({0,0,0},1e-35f);write("contrast.vdb",{fog});fail("contrast.vdb",Op::FogToSdf);
}
TEST_F(OpenVdbFastSweeping, DoubleWorldVectorExtensionAndNonzeroIsovalueRemainNative) {
  auto s=plane<openvdb::DoubleGrid>();for(auto it=s->beginValueAll();it;++it) it.setValue(double(*it)+9);s->tree().root().setBackground(29,false);
  auto f=openvdb::Vec3DGrid::create(openvdb::Vec3d(1));f->setName("field");f->setTransform(std::make_shared<openvdb::math::Transform>(s->transform()));
  for(int x=-5;x<=5;++x) for(int y=-3;y<=3;++y) for(int z=-3;z<=3;++z) f->tree().setValue({x,y,z},openvdb::Vec3d(x+std::ldexp(1.,-35),y+2*x,z-3*x));
  write("sdf.vdb",{s});write("field.vdb",{f});vdb::FastSweepingOptions o;o.isoValue=9;
  (void)vdb::WriteFastExtendedField(path("sdf.vdb"),path("field.vdb"),path("out.vdb"),false,o,{}, {},p);auto ext=read<openvdb::Vec3DGrid>("out.vdb","distance_extension");
  for(int x=-4;x<=4;++x) {auto v=ext->tree().getValue({x,0,0});EXPECT_NEAR(v.x(),.5+std::ldexp(1.,-35),1e-13);EXPECT_NEAR(v.y(),1,1e-13);EXPECT_NEAR(v.z(),-1.5,1e-13);}
}
TEST_F(OpenVdbFastSweeping, ExplicitFogTileExpansionAndDisconnectedSeededComponents) {
  auto fog=openvdb::FloatGrid::create(-1);fog->setName("fog");fog->setGridClass(openvdb::GRID_FOG_VOLUME);fog->tree().addTile(1,{0,0,0},1,true);write("tile-fog.vdb",{fog});fail("tile-fog.vdb",Op::FogToSdf);
  vdb::FastSweepingOptions o;o.voxelizeActiveTiles=true;
  (void)vdb::WriteFastSwept(path("tile-fog.vdb"),path("tile-out.vdb"),Op::FogToSdf,o,{},p);auto tile=read<openvdb::FloatGrid>("tile-out.vdb");EXPECT_EQ(tile->activeVoxelCount(),512u);EXPECT_LT(tile->tree().getValue({3,3,3}),-2);
  auto s=plane<openvdb::DoubleGrid>();
  for(int x=-5;x<=5;++x) for(int y=7;y<=13;++y) for(int z=-3;z<=3;++z) s->tree().setValueOff({x,y,z},(x-.5)*.5);
  for(int x=-4;x<=4;++x) for(int y=8;y<=12;++y) for(int z=-2;z<=2;++z) s->tree().setValue({x,y,z},(x-.5)*.5);
  write("two.vdb",{s});(void)vdb::WriteFastSwept(path("two.vdb"),path("two-out.vdb"),Op::ReinitializeSdf,{}, {},p);auto two=read<openvdb::DoubleGrid>("two-out.vdb");EXPECT_NEAR(two->tree().getValue({-4,10,0}),-1.125,1e-12);
}
TEST_F(OpenVdbFastSweeping, ExtensionMaskEmptyPrecisionFrameAndAffineFailuresLeaveNoOutput) {
  auto s=plane<openvdb::DoubleGrid>();auto f=field(*s);write("sdf.vdb",{s});write("field.vdb",{f});auto mask=openvdb::BoolGrid::create(false);mask->setTransform(std::make_shared<openvdb::math::Transform>(s->transform()));mask->setName("mask");write("empty-mask.vdb",{mask});vdb::FastSweepingOptions o;o.mask=path("empty-mask.vdb");
  EXPECT_THROW((void)vdb::WriteFastExtendedField(path("sdf.vdb"),path("field.vdb"),path("empty-out.vdb"),false,o,{}, {},p),std::exception);EXPECT_FALSE(std::filesystem::exists(path("empty-out.vdb")));
  auto different=plane<openvdb::FloatGrid>();write("float.vdb",{different});
  EXPECT_THROW((void)vdb::WriteFastExtendedField(path("sdf.vdb"),path("float.vdb"),path("precision.vdb"),false,{}, {},{},p),std::exception);EXPECT_FALSE(std::filesystem::exists(path("precision.vdb")));
  f->setIsInWorldSpace(false);write("local.vdb",{f});EXPECT_THROW((void)vdb::WriteFastExtendedField(path("sdf.vdb"),path("local.vdb"),path("local-out.vdb"),false,{}, {},{},p),std::exception);
  f->setIsInWorldSpace(true);f->transform().postTranslate(openvdb::Vec3d(1e-12,0,0));write("different.vdb",{f});EXPECT_THROW((void)vdb::WriteFastExtendedField(path("sdf.vdb"),path("different.vdb"),path("map-out.vdb"),false,{}, {},{},p),std::exception);
}
TEST_F(OpenVdbFastSweeping, AllNineNativeActivityTypesIgnorePayloadsForMasksAndTopology) {
  auto s=plane<openvdb::DoubleGrid>();write("sdf.vdb",{s});
  const auto make=[&]<class Grid>() -> openvdb::GridBase::Ptr {
    using V=typename Grid::ValueType;auto m=Grid::create(V{});m->setName("mask");m->setTransform(std::make_shared<openvdb::math::Transform>(s->transform()));
    for(int x=-3;x<=3;++x) for(int z=-2;z<=2;++z) {
      if constexpr(std::is_same_v<Grid,openvdb::MaskGrid>) m->tree().setValueOn({x,0,z});
      else m->tree().setValueOn({x,0,z},V{});
    }
    if constexpr(openvdb::VecTraits<V>::IsVec) {m->setIsInWorldSpace(false);m->setVectorType(openvdb::VEC_COVARIANT_NORMALIZE);}
    return m;
  };
  openvdb::GridPtrVec masks{make.template operator()<openvdb::BoolGrid>(),make.template operator()<openvdb::MaskGrid>(),make.template operator()<openvdb::FloatGrid>(),make.template operator()<openvdb::DoubleGrid>(),make.template operator()<openvdb::Int32Grid>(),make.template operator()<openvdb::Int64Grid>(),make.template operator()<openvdb::Vec3IGrid>(),make.template operator()<openvdb::Vec3SGrid>(),make.template operator()<openvdb::Vec3DGrid>()};
  for(const auto &mask:masks) {
    SCOPED_TRACE(mask->type());write("mask.vdb",{mask});vdb::FastSweepingOptions o;o.mask=path("mask.vdb");
    (void)vdb::WriteFastSwept(path("sdf.vdb"),path("masked.vdb"),Op::ReinitializeSdf,o,{},p);EXPECT_EQ(read<openvdb::DoubleGrid>("masked.vdb")->activeVoxelCount(),35u);std::filesystem::remove(path("masked.vdb"));
    (void)vdb::WriteFastSwept(path("mask.vdb"),path("topology.vdb"),Op::TopologyToSdf,{}, {},p);auto topo=read<openvdb::FloatGrid>("topology.vdb");EXPECT_LT(topo->tree().getValue({0,0,0}),0);EXPECT_TRUE(topo->isInWorldSpace());EXPECT_EQ(topo->getVectorType(),openvdb::VEC_INVARIANT);std::filesystem::remove(path("topology.vdb"));
  }
}
TEST_F(OpenVdbFastSweeping, SmallPersistedUniformScaleSupportsMaskAndFieldWithoutAffineReconstruction) {
  auto s=plane<openvdb::DoubleGrid>(.25);std::stringstream record(std::ios::in|std::ios::out|std::ios::binary);
  const double h=1e-6;openvdb::writeString(record,openvdb::math::UniformScaleMap::mapType());
  for(double value:{h,h,1/h,1e12,.5/h}) openvdb::Vec3d(value).write(record);
  record.seekg(0);openvdb::io::setCurrentVersion(record);s->transform().read(record);
  auto mask=openvdb::BoolGrid::create(false);mask->setName("mask");mask->setTransform(std::make_shared<openvdb::math::Transform>(s->transform()));
  for(int x=-3;x<=3;++x) for(int z=-2;z<=2;++z) mask->tree().setValueOn({x,0,z});
  auto f=field(*s);write("sdf.vdb",{s});write("mask.vdb",{mask});write("field.vdb",{f});vdb::FastSweepingOptions o;o.mask=path("mask.vdb");
  const auto report=vdb::WriteFastExtendedField(path("sdf.vdb"),path("field.vdb"),path("small.vdb"),false,o,{}, {},p);EXPECT_EQ(report.grid.voxelSize,h);
  EXPECT_NEAR(read<openvdb::DoubleGrid>("small.vdb")->tree().getValue({-3,0,0}),-3.5e-6,1e-18);EXPECT_NEAR(read<openvdb::DoubleGrid>("small.vdb","distance_extension")->tree().getValue({-3,0,0}),3.5,1e-12);
}
TEST_F(OpenVdbFastSweeping, DisabledDelayedLoadingIsRejectedBeforeGridMaterialization) {
  auto s=plane<openvdb::FloatGrid>();write("sdf.vdb",{s});
  const char *old=std::getenv("OPENVDB_DISABLE_DELAYED_LOAD");const std::string original=old?old:"";const bool existed=old!=nullptr;
  struct Restore {std::string value;bool existed;~Restore(){
#ifdef _WIN32
    _putenv_s("OPENVDB_DISABLE_DELAYED_LOAD",existed?value.c_str():"");
#else
    if(existed) setenv("OPENVDB_DISABLE_DELAYED_LOAD",value.c_str(),1);else unsetenv("OPENVDB_DISABLE_DELAYED_LOAD");
#endif
  }} restore{original,existed};
#ifdef _WIN32
  _putenv_s("OPENVDB_DISABLE_DELAYED_LOAD","1");
#else
  setenv("OPENVDB_DISABLE_DELAYED_LOAD","1",1);
#endif
  try {(void)vdb::WriteFastSwept(path("sdf.vdb"),path("disabled.vdb"),Op::ReinitializeSdf,{}, {},p);FAIL();}
  catch(const std::invalid_argument &error) {EXPECT_NE(std::string(error.what()).find("OPENVDB_DISABLE_DELAYED_LOAD"),std::string::npos);}
  EXPECT_FALSE(std::filesystem::exists(path("disabled.vdb")));
}
TEST_F(OpenVdbFastSweeping, DoubleNonzeroSeedUnitConditioningUnderflowFailsBeforePublication) {
  auto s=plane<openvdb::DoubleGrid>();s->setTransform(openvdb::math::Transform::createLinearTransform(1e6));s->tree().setValue({0,0,0},1e-320);write("sdf.vdb",{s});
  try {(void)vdb::WriteFastSwept(path("sdf.vdb"),path("underflow.vdb"),Op::DilateSdf,{}, {},p);FAIL();}
  catch(const std::invalid_argument &error) {EXPECT_NE(std::string(error.what()).find("precision range"),std::string::npos);}
  EXPECT_FALSE(std::filesystem::exists(path("underflow.vdb")));
}
TEST_F(OpenVdbFastSweeping, OptionsSemanticNonfiniteTransformAndGrowthBudgetsFailBeforePublication) {
  auto s=plane<openvdb::FloatGrid>();write("sdf.vdb",{s});vdb::FastSweepingOptions o;o.iterations=0;fail("sdf.vdb",Op::ReinitializeSdf,o);o={};o.maxWorkItems=100;fail("sdf.vdb",Op::ReinitializeSdf,o);
  auto limits=p;limits.maxVoxelCount=100;fail("sdf.vdb",Op::DilateSdf,{},&limits);limits=p;limits.maxInputBytes=1024;fail("sdf.vdb",Op::ReinitializeSdf,{},&limits);
  s->tree().setValueOff({0,0,0},std::numeric_limits<float>::infinity());write("nan.vdb",{s});fail("nan.vdb",Op::ReinitializeSdf);
  s=plane<openvdb::FloatGrid>();auto t=openvdb::math::Transform::createLinearTransform(1);t->postScale(openvdb::Vec3d(1,2,1));s->setTransform(t);write("map.vdb",{s});fail("map.vdb",Op::ReinitializeSdf);
  s=plane<openvdb::FloatGrid>();s->tree().addTile(1,{8,0,0},1,true);write("tile.vdb",{s});fail("tile.vdb",Op::ReinitializeSdf);write("missing-stats.vdb",{s},false);fail("missing-stats.vdb",Op::ReinitializeSdf);
}
TEST_F(OpenVdbFastSweeping, ExactDistantMapSelectorsUnicodeAndExclusivePublication) {
  auto s=plane<openvdb::DoubleGrid>(.1);s->transform().postTranslate(openvdb::Vec3d(1e8,-2e8,3e8));write("sdf.vdb",{s});
  const auto unicodeParent=directory/std::filesystem::path(L"\u5bc6\u5ea6-\u00e9");ASSERT_TRUE(std::filesystem::create_directory(unicodeParent));
  auto unicode=unicodeParent/"output.vdb";auto unicodeInput=unicodeParent/"input.vdb";std::filesystem::copy_file(path("sdf.vdb"),unicodeInput);
  (void)vdb::WriteFastSwept(unicodeInput,unicode,Op::ReinitializeSdf,{},"descriptor:source",p);
  // Read Unicode output through a native snapshot.
  std::filesystem::copy_file(unicode,path("readback.vdb"));auto result=read<openvdb::DoubleGrid>("readback.vdb");
  EXPECT_EQ(result->transform().baseMap()->getAffineMap()->getMat4(),s->transform().baseMap()->getAffineMap()->getMat4());
  const auto size=std::filesystem::file_size(unicode);EXPECT_THROW((void)vdb::WriteFastSwept(path("sdf.vdb"),unicode,Op::ReinitializeSdf,{}, {},p),std::exception);EXPECT_EQ(std::filesystem::file_size(unicode),size);
  auto other=plane<openvdb::DoubleGrid>();write("duplicates.vdb",{s,other});fail("duplicates.vdb",Op::ReinitializeSdf,{},nullptr,"source");
}
#endif
} // namespace
