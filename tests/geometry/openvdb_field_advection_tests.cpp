#include "Container/geometry/OpenVdbFieldAdvection.h"
#include <gtest/gtest.h>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <sstream>
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
#include <openvdb/openvdb.h>
#include <openvdb/io/File.h>
#endif
namespace {
namespace vdb=container::geometry::vdb;
using Scheme=vdb::FieldAdvectionScheme;using Limiter=vdb::FieldAdvectionLimiter;
TEST(OpenVdbFieldAdvectionAvailability, DisabledPrecedesPathsAndOptions) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  GTEST_SKIP();
#else
  vdb::FieldAdvectionOptions o;o.steps=0;
  try {(void)vdb::WriteAdvectedField("missing","missing",{},o,{}, {},{.maxInputBytes=0});FAIL();}
  catch(const std::runtime_error &e) {EXPECT_NE(std::string(e.what()).find("ENABLE_OPENVDB=ON"),std::string::npos);}
#endif
}
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
class OpenVdbFieldAdvection:public ::testing::Test {
protected:
  std::filesystem::path directory;vdb::ProcessingOptions p;
  void SetUp() override {openvdb::initialize();static std::atomic_uint64_t serial{};directory=std::filesystem::temp_directory_path()/("container-field-advection-tests-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+"-"+std::to_string(++serial));ASSERT_TRUE(std::filesystem::create_directory(directory));p.gridName="advected";p.maxInputBytes=2ull*1024*1024*1024;}
  void TearDown() override {std::error_code ec;std::filesystem::remove_all(directory,ec);}
  auto path(const std::string &s) {return directory/s;}
  void write(const std::string &name,const openvdb::GridPtrVec &grids,bool stats=true) {
    openvdb::MetaMap meta;meta.insertMeta("scene",openvdb::StringMetadata("retained"));openvdb::io::File f(path(name).string());f.setCompression(openvdb::io::COMPRESS_ZIP);f.setGridStatsMetadataEnabled(stats);f.write(grids,meta);f.close();
  }
  template<class Grid> auto read(const std::string &file) {
    openvdb::io::File f(path(file).string());f.open(false);EXPECT_EQ(f.getMetadata()->metaValue<std::string>("scene"),"retained");auto g=openvdb::gridPtrCast<Grid>(f.readGrid("advected"));f.close();return g;
  }
  std::string bytes(const std::string &file) {std::ifstream f(path(file),std::ios::binary);return {std::istreambuf_iterator<char>(f),{}};}
  template<class Grid> auto impulse(typename Grid::ValueType value=typename Grid::ValueType(1),typename Grid::ValueType background=typename Grid::ValueType(0)) {
    auto g=Grid::create(background);g->setName("source");g->tree().setValue({0,0,0},value);g->insertMeta("tag",openvdb::StringMetadata("source preserved"));return g;
  }
  auto velocity(openvdb::Vec3d v={1,0,0}) {auto g=openvdb::Vec3DGrid::create(v);g->setName("velocity");g->setVectorType(openvdb::VEC_CONTRAVARIANT_RELATIVE);return g;}
  void smallScale(openvdb::GridBase &g) {
    // The scalar factory applies an absolute determinant threshold. Persisted
    // stock maps remain well conditioned at h=1e-6, so use the public record API.
    constexpr double h=1e-6;std::stringstream record(std::ios::in|std::ios::out|std::ios::binary);
    openvdb::writeString(record,openvdb::math::UniformScaleMap::mapType());
    for(double value:{h,h,1/h,1e12,.5/h}) openvdb::Vec3d(value).write(record);
    record.seekg(0);openvdb::io::setCurrentVersion(record);g.transform().read(record);
  }
  void failure(const vdb::FieldAdvectionOptions &o={},const vdb::ProcessingOptions *limits=nullptr) {
    static uint32_t n{};const auto name="failed-"+std::to_string(++n)+".vdb";
    EXPECT_THROW((void)vdb::WriteAdvectedField(path("source.vdb"),path("velocity.vdb"),path(name),o,{}, {},limits?*limits:p),std::exception);EXPECT_FALSE(std::filesystem::exists(path(name)));
  }
  template<class Grid> void integerTranslations() {
    auto g=impulse<Grid>(typename Grid::ValueType(10),typename Grid::ValueType(3));g->tree().setValueOff({-20,0,0},typename Grid::ValueType(-7));g->tree().setValueOff({5,5,5},typename Grid::ValueType(-0.0));write("source.vdb",{g});write("velocity.vdb",{velocity()});const auto original=bytes("source.vdb");
    for(auto scheme:{Scheme::Semi,Scheme::Midpoint,Scheme::Rk3,Scheme::Rk4,Scheme::MacCormack,Scheme::Bfecc}) {
      SCOPED_TRACE(int(scheme));vdb::FieldAdvectionOptions o;o.timeStep=1;o.scheme=scheme;const auto name="integer-"+std::to_string(int(scheme))+".vdb";
      const auto r=vdb::WriteAdvectedField(path("source.vdb"),path("velocity.vdb"),path(name),o,{}, {},p);auto out=read<Grid>(name);
      EXPECT_EQ(out->tree().getValue({1,0,0}),typename Grid::ValueType(10));EXPECT_TRUE(out->tree().isValueOn({1,0,0}));EXPECT_FALSE(out->tree().isValueOn({0,0,0}));EXPECT_EQ(out->activeVoxelCount(),1u);
      EXPECT_EQ(out->tree().getValue({-20,0,0}),typename Grid::ValueType(-7));EXPECT_FALSE(out->tree().isValueOn({-20,0,0}));EXPECT_TRUE(std::signbit(out->tree().getValue({5,5,5})));
      EXPECT_EQ(r.executedSteps,1u);EXPECT_FALSE(r.noOp);EXPECT_EQ(out->metaValue<std::string>("tag"),"source preserved");EXPECT_FALSE(out->saveFloatAsHalf());
    } EXPECT_EQ(bytes("source.vdb"),original);
  }
};
TEST_F(OpenVdbFieldAdvection, SixSchemesTranslateFloatExactlyWithNonzeroBackgroundAndInactiveBits) {integerTranslations<openvdb::FloatGrid>();}
TEST_F(OpenVdbFieldAdvection, SixSchemesTranslateDoubleExactlyWithNonzeroBackgroundAndInactiveBits) {integerTranslations<openvdb::DoubleGrid>();}
TEST_F(OpenVdbFieldAdvection, TinyNativeDoubleChannelSurvivesWithoutApproximateBackgroundPruning) {
  auto s=impulse<openvdb::DoubleGrid>(1e-300);write("source.vdb",{s});write("velocity.vdb",{velocity()});vdb::FieldAdvectionOptions o;o.timeStep=1;o.scheme=Scheme::Bfecc;
  (void)vdb::WriteAdvectedField(path("source.vdb"),path("velocity.vdb"),path("tiny.vdb"),o,{}, {},p);auto out=read<openvdb::DoubleGrid>("tiny.vdb");EXPECT_EQ(out->tree().getValue({1,0,0}),1e-300);EXPECT_TRUE(out->tree().isValueOn({1,0,0}));
}
TEST_F(OpenVdbFieldAdvection, FractionalTranslationHasAnalyticDiffusionAndCorrectionProfiles) {
  auto s=impulse<openvdb::DoubleGrid>();s->setGridClass(openvdb::GRID_FOG_VOLUME);write("source.vdb",{s});write("velocity.vdb",{velocity()});
  for(auto scheme:{Scheme::Semi,Scheme::MacCormack,Scheme::Bfecc}) {
    vdb::FieldAdvectionOptions o;o.timeStep=.5;o.scheme=scheme;o.limiter=Limiter::None;const auto name="profile-"+std::to_string(int(scheme))+".vdb";auto report=vdb::WriteAdvectedField(path("source.vdb"),path("velocity.vdb"),path(name),o,{}, {},p);auto out=read<openvdb::DoubleGrid>(name);
    if(scheme==Scheme::Semi) {EXPECT_DOUBLE_EQ(out->tree().getValue({0,0,0}),.5);EXPECT_DOUBLE_EQ(out->tree().getValue({1,0,0}),.5);EXPECT_FALSE(report.fogClassCleared);}
    else if(scheme==Scheme::MacCormack) {EXPECT_DOUBLE_EQ(out->tree().getValue({-1,0,0}),-.125);EXPECT_DOUBLE_EQ(out->tree().getValue({0,0,0}),.75);EXPECT_DOUBLE_EQ(out->tree().getValue({1,0,0}),.375);EXPECT_TRUE(report.fogClassCleared);EXPECT_GT(report.overshootVoxelCount,0u);}
    else {EXPECT_DOUBLE_EQ(out->tree().getValue({-1,0,0}),-.0625);EXPECT_DOUBLE_EQ(out->tree().getValue({0,0,0}),.5625);EXPECT_DOUBLE_EQ(out->tree().getValue({1,0,0}),.5625);EXPECT_DOUBLE_EQ(out->tree().getValue({2,0,0}),-.0625);EXPECT_TRUE(report.fogClassCleared);}
    if(scheme!=Scheme::Semi) {EXPECT_GT(report.maximumRoundTripCorrection[0],0);EXPECT_EQ(out->getGridClass(),openvdb::GRID_UNKNOWN);}
  }
}
TEST_F(OpenVdbFieldAdvection, ClampAndRevertSuppressOvershootAndKeepValidFogClass) {
  auto s=impulse<openvdb::FloatGrid>();s->setGridClass(openvdb::GRID_FOG_VOLUME);write("source.vdb",{s});write("velocity.vdb",{velocity()});
  for(auto limiter:{Limiter::Clamp,Limiter::Revert}) for(auto scheme:{Scheme::MacCormack,Scheme::Bfecc}) {
    vdb::FieldAdvectionOptions o;o.timeStep=.5;o.scheme=scheme;o.limiter=limiter;const auto name="limit-"+std::to_string(int(limiter))+"-"+std::to_string(int(scheme))+".vdb";auto r=vdb::WriteAdvectedField(path("source.vdb"),path("velocity.vdb"),path(name),o,{}, {},p);auto out=read<openvdb::FloatGrid>(name);
    EXPECT_GE(r.outputMinimum[0],0);EXPECT_LE(r.outputMaximum[0],1);EXPECT_FALSE(r.fogClassCleared);EXPECT_EQ(out->getGridClass(),openvdb::GRID_FOG_VOLUME);EXPECT_GT(r.overshootVoxelCount,0u);if(limiter==Limiter::Clamp) EXPECT_GT(r.clampedVoxelCount,0u);else EXPECT_GT(r.revertedVoxelCount,0u);
  }
}
TEST_F(OpenVdbFieldAdvection, WorldVectorsMatchIndependentScalarChannelsUnderComponentwiseLimiters) {
  auto v=openvdb::Vec3DGrid::create(openvdb::Vec3d(0));v->setName("source");v->setVectorType(openvdb::VEC_CONTRAVARIANT_RELATIVE);
  for(int x=-2;x<=2;++x) v->tree().setValue({x,0,0},openvdb::Vec3d(x==0?1:0,x==1?-3:0,x==-1?7:0));write("vector.vdb",{v});write("velocity.vdb",{velocity()});vdb::FieldAdvectionOptions o;o.timeStep=.5;o.scheme=Scheme::MacCormack;o.limiter=Limiter::Clamp;
  (void)vdb::WriteAdvectedField(path("vector.vdb"),path("velocity.vdb"),path("vector-out.vdb"),o,{}, {},p);auto out=read<openvdb::Vec3DGrid>("vector-out.vdb");EXPECT_EQ(out->getVectorType(),v->getVectorType());
  for(size_t channel=0;channel<3;++channel) {
    auto s=openvdb::DoubleGrid::create(0);s->setName("source");for(auto it=v->cbeginValueOn();it;++it) s->tree().setValue(it.getCoord(),(*it)[channel]);write("scalar.vdb",{s});const auto name="channel-"+std::to_string(channel)+".vdb";(void)vdb::WriteAdvectedField(path("scalar.vdb"),path("velocity.vdb"),path(name),o,{}, {},p);auto actual=read<openvdb::DoubleGrid>(name);
    for(int x=-5;x<=5;++x) EXPECT_DOUBLE_EQ(out->tree().getValue({x,0,0})[channel],actual->tree().getValue({x,0,0}));
  }
  auto f=impulse<openvdb::Vec3SGrid>({1,4,-7},{-3,2,11});write("float-vector.vdb",{f});o.timeStep=1;o.scheme=Scheme::Rk4;(void)vdb::WriteAdvectedField(path("float-vector.vdb"),path("velocity.vdb"),path("float-vector-out.vdb"),o,{}, {},p);EXPECT_EQ(read<openvdb::Vec3SGrid>("float-vector-out.vdb")->tree().getValue({1,0,0}),openvdb::Vec3f(1,4,-7));
}
TEST_F(OpenVdbFieldAdvection, FinalMaskKeepsUnselectedBitsWithoutClippingIntermediateTransport) {
  auto s=impulse<openvdb::DoubleGrid>();s->tree().setValueOff({5,5,5},-0.0);auto m=openvdb::BoolGrid::create(false);m->setName("mask");m->tree().setValue({2,0,0},false);write("source.vdb",{s});write("velocity.vdb",{velocity()});write("mask.vdb",{m});vdb::FieldAdvectionOptions o;o.timeStep=1;o.steps=2;o.scheme=Scheme::Semi;o.mask=path("mask.vdb");
  auto r=vdb::WriteAdvectedField(path("source.vdb"),path("velocity.vdb"),path("masked.vdb"),o,{}, {},p);auto out=read<openvdb::DoubleGrid>("masked.vdb");EXPECT_DOUBLE_EQ(out->tree().getValue({2,0,0}),.25);EXPECT_TRUE(out->tree().isValueOn({2,0,0}));EXPECT_DOUBLE_EQ(out->tree().getValue({0,0,0}),1);EXPECT_TRUE(out->tree().isValueOn({0,0,0}));EXPECT_FALSE(out->tree().isValueOn({1,0,0}));EXPECT_TRUE(std::signbit(out->tree().getValue({5,5,5})));EXPECT_EQ(r.executedSteps,2u);EXPECT_TRUE(r.maskUsed);
}
TEST_F(OpenVdbFieldAdvection, NativeMacNegativeFacesScaleAndRotationHaveIndependentVelocityOracle) {
  auto s=openvdb::DoubleGrid::create(0);s->setName("source");for(int x=-4;x<=4;++x) for(int y=-4;y<=4;++y) for(int z=-2;z<=2;++z) s->tree().setValue({x,y,z},double(y));
  auto v=openvdb::Vec3DGrid::create(openvdb::Vec3d(0));v->setName("velocity");v->setGridClass(openvdb::GRID_STAGGERED);v->setVectorType(openvdb::VEC_CONTRAVARIANT_RELATIVE);v->setIsInWorldSpace(false);v->insertMeta("container:mac_layout",openvdb::StringMetadata("negative_faces"));v->insertMeta("container:velocity_units",openvdb::StringMetadata("voxels_per_second"));v->setTransform(openvdb::math::Transform::createLinearTransform(2));v->transform().postRotate(std::acos(-1.)/2,openvdb::math::Z_AXIS);
  for(int x=-5;x<=5;++x) for(int y=-5;y<=5;++y) for(int z=-2;z<=2;++z) v->tree().setValue({x,y,z},openvdb::Vec3d(double(x),0,0));write("source.vdb",{s});write("velocity.vdb",{v});vdb::FieldAdvectionOptions o;o.timeStep=.25;o.scheme=Scheme::Semi;o.velocityLayout=vdb::FieldVelocityLayout::NativeMacIndex;
  (void)vdb::WriteAdvectedField(path("source.vdb"),path("velocity.vdb"),path("mac.vdb"),o,{}, {},p);auto out=read<openvdb::DoubleGrid>("mac.vdb");EXPECT_NEAR(out->tree().getValue({0,0,0}),-.25,1e-12);EXPECT_NEAR(out->tree().getValue({0,1,0}),.5,1e-12);
}
TEST_F(OpenVdbFieldAdvection, IndependentAffineVelocityAndInactiveValuesParticipateInWorldUnits) {
  auto s=openvdb::DoubleGrid::create(0);s->setName("source");s->setTransform(openvdb::math::Transform::createLinearTransform(.5));s->transform().postTranslate(openvdb::Vec3d(1e8,-2e8,3e8));for(int x=-4;x<=4;++x) for(int y=-2;y<=2;++y) for(int z=-2;z<=2;++z) s->tree().setValue({x,y,z},.5*x);
  auto v=velocity({0,0,0});v->setTransform(std::make_shared<openvdb::math::Transform>(s->transform()));v->tree().setValueOff({0,0,0},openvdb::Vec3d(4,0,0));write("source.vdb",{s});write("velocity.vdb",{v});vdb::FieldAdvectionOptions o;o.timeStep=.125;o.scheme=Scheme::Semi;
  const auto r=vdb::WriteAdvectedField(path("source.vdb"),path("velocity.vdb"),path("inactive-flow.vdb"),o,{}, {},p);EXPECT_GE(r.maximumWorldSpeed,4);EXPECT_NEAR(read<openvdb::DoubleGrid>("inactive-flow.vdb")->tree().getValue({0,0,0}),-.5,1e-12);
  v=velocity({0,0,0});v->setTransform(openvdb::math::Transform::createLinearTransform(.25));v->transform().postTranslate(openvdb::Vec3d(1e8+.125,-2e8,3e8));for(int x=-8;x<=8;++x) for(int y=-4;y<=4;++y) for(int z=-4;z<=4;++z) v->tree().setValue({x,y,z},openvdb::Vec3d(1+.125+.25*x,0,0));write("velocity.vdb",{v});(void)vdb::WriteAdvectedField(path("source.vdb"),path("velocity.vdb"),path("independent-flow.vdb"),o,{}, {},p);EXPECT_NEAR(read<openvdb::DoubleGrid>("independent-flow.vdb")->tree().getValue({0,0,0}),-.125,1e-12);
}
TEST_F(OpenVdbFieldAdvection, RkOrderConvergesForIndependentAnalyticRigidRotation) {
  auto s=openvdb::DoubleGrid::create(0);s->setName("source");for(int x=-3;x<=3;++x) for(int y=-3;y<=3;++y) for(int z=-2;z<=2;++z) s->tree().setValue({x,y,z},double(x));auto v=velocity({0,0,0});for(int x=-8;x<=8;++x) for(int y=-8;y<=8;++y) for(int z=-2;z<=2;++z) v->tree().setValue({x,y,z},openvdb::Vec3d(-double(y),double(x),0));write("source.vdb",{s});write("velocity.vdb",{v});
  std::array<double,4> errors{};size_t n=0;for(auto scheme:{Scheme::Semi,Scheme::Midpoint,Scheme::Rk3,Scheme::Rk4}) {vdb::FieldAdvectionOptions o;o.timeStep=.4;o.maxCfl=16;o.scheme=scheme;const auto name="rotation-"+std::to_string(n)+".vdb";(void)vdb::WriteAdvectedField(path("source.vdb"),path("velocity.vdb"),path(name),o,{}, {},p);errors[n++]=std::abs(read<openvdb::DoubleGrid>(name)->tree().getValue({1,0,0})-std::cos(.4));}
  EXPECT_LT(errors[1],errors[0]);EXPECT_LT(errors[3],errors[1]);EXPECT_LT(errors[3],1e-4);
}
TEST_F(OpenVdbFieldAdvection, NoOpsRetainExactNativeTilesValuesMapsAndExclusiveUnicodeOutput) {
  auto s=impulse<openvdb::DoubleGrid>(std::nextafter(3.,4.),3.);s->tree().addTile(1,{8,0,0},-7.,true);s->tree().setValueOff({-5,0,0},-0.);s->setTransform(openvdb::math::Transform::createLinearTransform(.1));s->transform().postTranslate(openvdb::Vec3d(1e8,-2e8,3e8));write("source.vdb",{s});write("velocity.vdb",{velocity()});vdb::FieldAdvectionOptions o;o.timeStep=0;
  auto parent=directory/std::filesystem::path(L"\u5bf9\u6d41-\u00e9");ASSERT_TRUE(std::filesystem::create_directory(parent));auto output=parent/"no-op.vdb";auto r=vdb::WriteAdvectedField(path("source.vdb"),path("velocity.vdb"),output,o,{}, {},p);EXPECT_TRUE(r.noOp);std::filesystem::copy_file(output,path("readback.vdb"));auto out=read<openvdb::DoubleGrid>("readback.vdb");EXPECT_TRUE(out->tree().hasSameTopology(s->tree()));EXPECT_EQ(out->transform().baseMap()->getAffineMap()->getMat4(),s->transform().baseMap()->getAffineMap()->getMat4());
  for(auto it=s->cbeginValueAll();it;++it) {EXPECT_EQ(std::bit_cast<uint64_t>(double(*it)),std::bit_cast<uint64_t>(out->tree().getValue(it.getCoord())));EXPECT_EQ(it.isValueOn(),out->tree().isValueOn(it.getCoord()));}
  const auto size=std::filesystem::file_size(output);EXPECT_THROW((void)vdb::WriteAdvectedField(path("source.vdb"),path("velocity.vdb"),output,o,{}, {},p),std::exception);EXPECT_EQ(std::filesystem::file_size(output),size);
}
TEST_F(OpenVdbFieldAdvection, InvalidOptionsSemanticsNonfiniteCflHalosAndBudgetsRejectBeforePublication) {
  auto s=impulse<openvdb::DoubleGrid>();write("source.vdb",{s});auto v=velocity();write("velocity.vdb",{v});vdb::FieldAdvectionOptions o;o.steps=0;failure(o);o={};o.timeStep=10;o.maxCfl=1;failure(o);o={};o.maxWorkItems=10;failure(o);o={};o.timeStep=1e-320;failure(o);
  auto limits=p;limits.maxInputBytes=1024;failure({},&limits);limits=p;limits.maxVoxelCount=512;failure({},&limits);
  s->tree().setValueOff({1,0,0},std::numeric_limits<double>::infinity());write("source.vdb",{s});failure();s=impulse<openvdb::DoubleGrid>();s->setGridClass(openvdb::GRID_LEVEL_SET);write("source.vdb",{s});failure();
  s=impulse<openvdb::DoubleGrid>();write("source.vdb",{s});v->setGridClass(openvdb::GRID_STAGGERED);write("velocity.vdb",{v});failure();v=velocity();v->tree().setValueOff({0,0,0},openvdb::Vec3d(std::numeric_limits<double>::quiet_NaN()));write("velocity.vdb",{v});failure();
}
TEST_F(OpenVdbFieldAdvection, NineMaskActivityTypesIgnorePayloadAndFloatVelocitySelectionSkipsScalars) {
  write("source.vdb",{impulse<openvdb::DoubleGrid>()});auto v=openvdb::Vec3SGrid::create(openvdb::Vec3f(1,0,0));v->setName("velocity");write("velocity.vdb",{impulse<openvdb::FloatGrid>(),v});
  const auto check=[&]<class Grid>(typename Grid::ValueType payload) {
    SCOPED_TRACE(Grid::gridType());auto mask=Grid::create();mask->setName("mask");if constexpr(std::is_same_v<Grid,openvdb::MaskGrid>) mask->tree().setValueOn({1,0,0});else mask->tree().setValue({1,0,0},payload);write("mask.vdb",{mask});vdb::FieldAdvectionOptions o;o.timeStep=1;o.scheme=Scheme::Semi;o.mask=path("mask.vdb");
    static unsigned count{};const auto output="mask-"+std::string(Grid::gridType())+"-"+std::to_string(++count)+".vdb";
    const auto r=vdb::WriteAdvectedField(path("source.vdb"),path("velocity.vdb"),path(output),o,{}, {},p);auto result=read<openvdb::DoubleGrid>(output);
    EXPECT_DOUBLE_EQ(result->tree().getValue({1,0,0}),1);EXPECT_TRUE(result->tree().isValueOn({1,0,0}));EXPECT_DOUBLE_EQ(result->tree().getValue({0,0,0}),1);EXPECT_TRUE(result->tree().isValueOn({0,0,0}));EXPECT_TRUE(r.maskUsed);
  };
  check.template operator()<openvdb::BoolGrid>(false);check.template operator()<openvdb::MaskGrid>(false);
  check.template operator()<openvdb::Int32Grid>(0);check.template operator()<openvdb::Int64Grid>(-1);
  check.template operator()<openvdb::FloatGrid>(std::numeric_limits<float>::quiet_NaN());check.template operator()<openvdb::DoubleGrid>(std::numeric_limits<double>::quiet_NaN());
  check.template operator()<openvdb::Vec3IGrid>(openvdb::Vec3i(0));check.template operator()<openvdb::Vec3SGrid>(openvdb::Vec3f(std::numeric_limits<float>::quiet_NaN()));check.template operator()<openvdb::Vec3DGrid>(openvdb::Vec3d(std::numeric_limits<double>::quiet_NaN()));
}
TEST_F(OpenVdbFieldAdvection, SignedTimeActiveTileConsentAndEmptyEffectiveMaskHaveExplicitSemantics) {
  auto s=openvdb::DoubleGrid::create(0);s->setName("source");s->tree().addTile(1,{8,0,0},2.,true);write("source.vdb",{s});write("velocity.vdb",{velocity()});vdb::FieldAdvectionOptions o;o.timeStep=-1;o.scheme=Scheme::Semi;failure(o);o.voxelizeActiveTiles=true;
  auto report=vdb::WriteAdvectedField(path("source.vdb"),path("velocity.vdb"),path("tile.vdb"),o,{}, {},p);auto out=read<openvdb::DoubleGrid>("tile.vdb");EXPECT_EQ(out->activeVoxelCount(),512u);EXPECT_DOUBLE_EQ(out->tree().getValue({7,0,0}),2);EXPECT_TRUE(out->tree().isValueOn({7,0,0}));EXPECT_FALSE(out->tree().isValueOn({15,0,0}));EXPECT_FALSE(report.noOp);
  for(bool empty:{true,false}) {
    auto m=openvdb::BoolGrid::create(false);m->setName("mask");if(!empty) m->tree().setValue({100,0,0},false);write("mask.vdb",{m});o.voxelizeActiveTiles=false;o.mask=path("mask.vdb");const auto name=empty?"empty-mask.vdb":"disjoint-mask.vdb";
    report=vdb::WriteAdvectedField(path("source.vdb"),path("velocity.vdb"),path(name),o,{}, {},p);out=read<openvdb::DoubleGrid>(name);EXPECT_TRUE(report.noOp);EXPECT_EQ(report.executedSteps,0u);EXPECT_TRUE(out->tree().hasSameTopology(s->tree()));EXPECT_TRUE(out->tree().hasActiveTiles());
  }
  o={};write("velocity.vdb",{velocity({0,0,0})});report=vdb::WriteAdvectedField(path("source.vdb"),path("velocity.vdb"),path("zero-speed.vdb"),o,{}, {},p);EXPECT_TRUE(report.noOp);EXPECT_TRUE(read<openvdb::DoubleGrid>("zero-speed.vdb")->tree().hasSameTopology(s->tree()));
  s=openvdb::DoubleGrid::create(3);s->setName("source");s->tree().setValueOff({0,0,0},-7);write("source.vdb",{s});write("velocity.vdb",{velocity()});report=vdb::WriteAdvectedField(path("source.vdb"),path("velocity.vdb"),path("empty-source.vdb"),o,{}, {},p);EXPECT_TRUE(report.noOp);EXPECT_DOUBLE_EQ(read<openvdb::DoubleGrid>("empty-source.vdb")->tree().getValue({0,0,0}),-7);
}
TEST_F(OpenVdbFieldAdvection, PromotedFloatVelocityRetainsSubnormalFractionBeforeWorldConversion) {
  auto s=openvdb::DoubleGrid::create(0);s->setName("source");smallScale(*s);s->tree().setValue({0,0,0},0);s->tree().setValue({1,0,0},1);write("source.vdb",{s});
  for(bool mac:{false,true}) {
    auto v=openvdb::Vec3SGrid::create(openvdb::Vec3f(0));v->setName("velocity");if(!mac) v->transform().postTranslate(openvdb::Vec3d(-.5,0,0));
    if(mac) {v->setGridClass(openvdb::GRID_STAGGERED);v->setIsInWorldSpace(false);v->setVectorType(openvdb::VEC_CONTRAVARIANT_RELATIVE);v->insertMeta("container:mac_layout",openvdb::StringMetadata("negative_faces"));v->insertMeta("container:velocity_units",openvdb::StringMetadata("voxels_per_second"));}
    v->tree().setValue({1,0,0},openvdb::Vec3f(-std::numeric_limits<float>::denorm_min(),0,0));write("velocity.vdb",{v});vdb::FieldAdvectionOptions o;o.timeStep=1;o.scheme=Scheme::Semi;const auto name=mac?"tiny-mac.vdb":"tiny-world.vdb";
    (void)vdb::WriteAdvectedField(path("source.vdb"),path("velocity.vdb"),path(name),o,{}, {},p);const double expected=.5*double(std::numeric_limits<float>::denorm_min())/1e-6;EXPECT_DOUBLE_EQ(read<openvdb::DoubleGrid>(name)->tree().getValue({0,0,0}),expected);
  }
  auto v=velocity({0,0,0});v->transform().postTranslate(openvdb::Vec3d(-.5,0,0));v->tree().setValue({1,0,0},openvdb::Vec3d(-std::numeric_limits<double>::denorm_min(),0,0));write("velocity.vdb",{v});vdb::FieldAdvectionOptions o;o.timeStep=1;o.scheme=Scheme::Semi;failure(o);
}
TEST_F(OpenVdbFieldAdvection, MissingStatisticsAndDisabledDelayedLoadingRejectBeforeMaterialization) {
  write("source.vdb",{impulse<openvdb::DoubleGrid>()});write("velocity.vdb",{velocity()},false);failure();write("velocity.vdb",{velocity()});write("source.vdb",{impulse<openvdb::DoubleGrid>()},false);failure();write("source.vdb",{impulse<openvdb::DoubleGrid>()});
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
  failure();
}
TEST_F(OpenVdbFieldAdvection, JointMapConditioningRejectsSmallFinalUlpWithLargeAffineUncertainty) {
  auto s=impulse<openvdb::DoubleGrid>();s->transform().postTranslate(openvdb::Vec3d(1e9,1e9,0));write("source.vdb",{s});auto v=velocity({0,0,0});
  openvdb::math::Mat4d map=openvdb::math::Mat4d::identity();map(0,0)=500000;map(0,1)=500000;map(1,0)=500000;map(1,1)=500000+1e-5;v->setTransform(openvdb::math::Transform::createLinearTransform(map));v->tree().setValue({2000,0,0},openvdb::Vec3d(.1,0,0));write("velocity.vdb",{v});vdb::FieldAdvectionOptions o;o.timeStep=.25;o.scheme=Scheme::Semi;
  try {(void)vdb::WriteAdvectedField(path("source.vdb"),path("velocity.vdb"),path("ill-conditioned.vdb"),o,{}, {},p);FAIL();}
  catch(const std::invalid_argument &error) {EXPECT_NE(std::string(error.what()).find("joint-map arithmetic uncertainty"),std::string::npos);}
  EXPECT_FALSE(std::filesystem::exists(path("ill-conditioned.vdb")));
}
#endif
} // namespace
