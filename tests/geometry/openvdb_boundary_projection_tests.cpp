#include "Container/geometry/OpenVdbBoundaryProjection.h"
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
#include <openvdb/openvdb.h>
#include <openvdb/io/File.h>
#include <openvdb/tools/Count.h>
#endif
namespace {
namespace vdb=container::geometry::vdb;
TEST(OpenVdbBoundaryProjectionAvailability, DisabledBeforeOptionsPathsAndBothInputForms){
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  GTEST_SKIP();
#else
  vdb::BoundaryProjectionOptions o;o.projection.timeStep=-1;o.maxWorkItems=0;vdb::ProcessingOptions p;p.maxInputBytes=0;
  for(bool replay:{false,true})try{if(replay)(void)vdb::WriteReprojectedBoundaryState({"missing"},"missing/out",o,p);else(void)vdb::WriteBoundaryProjectedVelocity({},"missing/out",o,p);FAIL();}catch(const std::runtime_error& e){EXPECT_NE(std::string(e.what()).find("ENABLE_OPENVDB=ON"),std::string::npos);}
#endif
}
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
using V=openvdb::Vec3d;using C=openvdb::Coord;using Mac=openvdb::Vec3DGrid;using Scalar=openvdb::DoubleGrid;using Mask=openvdb::BoolGrid;
class OpenVdbBoundaryProjection:public ::testing::Test {
protected:
  void SetUp()override{openvdb::initialize();static std::atomic_uint64_t serial{};directory=std::filesystem::temp_directory_path()/("container-boundary-projection-test-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+"-"+std::to_string(++serial));std::filesystem::create_directory(directory);o.projection.timeStep=1;o.projection.tolerance=1e-10;}
  void TearDown()override{std::error_code e;for(const auto& x:std::filesystem::directory_iterator(directory))std::filesystem::remove_all(x.path(),e);std::filesystem::remove(directory,e);}
  auto path(const std::string& n)const{return directory/n;}
  std::string bytes(const std::filesystem::path& n){std::ifstream s(n,std::ios::binary);return {std::istreambuf_iterator<char>(s),{}};}
  void write(const std::string& name,const openvdb::GridPtrVec& grids){openvdb::MetaMap meta;meta.insertMeta("scene",openvdb::Int64Metadata(INT64_C(9007199254740993)));openvdb::io::File file(path(name).string());file.setCompression(openvdb::io::COMPRESS_ZIP);file.write(grids,meta);file.close();}
  template<class Grid>typename Grid::Ptr read(const std::string& file,const std::string& name){openvdb::io::File input(path(file).string());input.open(false);auto result=openvdb::gridPtrCast<Grid>(input.readGrid(name));input.close();return result;}
  auto all(const std::string& name){openvdb::io::File file(path(name).string());file.open(false);auto grids=file.getGrids();file.close();return grids;}
  void channel(int count=3,double h=1,double wall=1,bool air=false,double predictor=0){
    count_=count;h_=h;auto transform=openvdb::math::Transform::createLinearTransform(h);
    auto fluid=Mask::create(false);fluid->setName("fluid");fluid->setTransform(transform->copy());fluid->insertMeta("artist",openvdb::StringMetadata(std::string("domain\0meta",11)));
    for(int x=0;x<count;++x)fluid->tree().setValueOn({x,0,0},true);
    auto exterior=Mask::create(false);exterior->setName(air?"air":"open");exterior->setTransform(transform->copy());exterior->tree().setValueOn({count,0,0},true);
    auto velocity=Mac::create(V(predictor,0,0));velocity->setName("wind");velocity->setIsInWorldSpace(true);velocity->setVectorType(openvdb::VEC_INVARIANT);velocity->insertMeta("artist",openvdb::StringMetadata("original wind"));
    write("domain.vdb",{fluid,exterior});write("wind.vdb",{velocity});in.fluid={path("domain.vdb"),"fluid"};in.velocity={path("wind.vdb"),"wind"};if(air)in.air={path("domain.vdb"),"air"};else in.open={path("domain.vdb"),"open"};o.wallVelocityWorld={wall,0,0};
  }
  auto run(const std::string& output="out.vdb"){return vdb::WriteBoundaryProjectedVelocity(in,path(output),o,p);}
  void expectChannel(const std::string& name,const std::vector<double>& expectedPressure,double expectedVelocity,double tolerance=1e-8){
    auto velocity=read<Mac>(name,"velocity");auto pressure=read<Scalar>(name,"pressure");ASSERT_TRUE(velocity);ASSERT_TRUE(pressure);
    for(int x=0;x<=count_;++x)EXPECT_NEAR(velocity->tree().getValue({x,0,0})[0],expectedVelocity,tolerance)<<x;
    for(int x=0;x<count_;++x){EXPECT_NEAR(pressure->tree().getValue({x,0,0}),expectedPressure[x],tolerance)<<x;V minus=velocity->tree().getValue({x,0,0});double divergence=0;for(int a=0;a<3;++a){C c(x,0,0);++c[a];divergence+=velocity->tree().getValue(c)[a]-minus[a];}EXPECT_NEAR(divergence,0,tolerance);}
  }
  void pressureDrive(){channel(3,2,0);auto masks=all("domain.vdb");auto exterior=openvdb::gridPtrCast<Mask>((*masks)[1]);exterior->tree().setValueOn({-1,0,0},true);write("both.vdb",*masks);in.open.file=path("both.vdb");
    auto pressure=Scalar::create(0);pressure->setName("pD");pressure->setTransform(openvdb::math::Transform::createLinearTransform(2));for(int x=-4;x<8;++x)pressure->tree().setValue({x,0,0},1.5-x);write("pD.vdb",{pressure});in.pressure={path("pD.vdb"),"pD"};o.projection.timeStep=.25;
  }
  std::filesystem::path directory;vdb::BoundaryProjectionInputs in;vdb::BoundaryProjectionOptions o;vdb::ProcessingOptions p;int count_=3;double h_=1;
};
TEST_F(OpenVdbBoundaryProjection, PhysicalOuterFaceAndAirCenterUseDifferentDistancesAndPhysicalUnits){
  for(bool air:{false,true}){in={};o={};o.projection.timeStep=1;o.projection.tolerance=1e-10;channel(3,1,1,air);const std::string out=air?"air.vdb":"face.vdb";const auto r=run(out);EXPECT_TRUE(r.projection.converged);EXPECT_EQ(r.dirichletComponents,1);EXPECT_EQ(r.neumannComponents,0);EXPECT_EQ(r.openFacePressureFaces,air?0:1);EXPECT_EQ(r.airCellPressureFaces,air?1:0);expectChannel(out,air?std::vector<double>{3,2,1}:std::vector<double>{2.5,1.5,.5},1);}
  in={};channel(2,2,2,true);o.projection.timeStep=.5;o.ambientPressure=7;const auto r=run("units.vdb");EXPECT_LE(r.projection.residual,o.projection.tolerance);expectChannel("units.vdb",{23,15},1);
}
TEST_F(OpenVdbBoundaryProjection, NonuniformPressureDrivesZeroDivergenceAndReplayAddsFreshImpulse){
  pressureDrive();const auto original=bytes(path("domain.vdb"));const auto r=run();EXPECT_EQ(r.projection.maxDivergenceBefore,0);EXPECT_GT(r.totalIterations,0);expectChannel("out.vdb",{1.5,.5,-.5},.0625);
  vdb::BoundaryProjectionReplay replay;replay.state=path("out.vdb");vdb::BoundaryProjectionOptions replayOptions;replayOptions.projection=o.projection;const auto repeated=vdb::WriteReprojectedBoundaryState(replay,path("repeated.vdb"),replayOptions,p);EXPECT_TRUE(repeated.replayed);expectChannel("repeated.vdb",{1.5,.5,-.5},.125);EXPECT_EQ(original,bytes(path("domain.vdb")));
  auto grids=all("out.vdb");for(auto& grid:*grids)if(grid->getName()=="velocity"){auto v=openvdb::gridPtrCast<Mac>(grid);for(auto i=v->beginValueAll();i;++i)i.setValue(V(0));}write("same-predictor.vdb",*grids);replay.state=path("same-predictor.vdb");(void)vdb::WriteReprojectedBoundaryState(replay,path("same-replay.vdb"),replayOptions,p);expectChannel("same-replay.vdb",{1.5,.5,-.5},.0625);
}
TEST_F(OpenVdbBoundaryProjection, ClosedBalancedMovingWallsHaveGaugeAndUniformTranslationIsUnchanged){
  channel(3,1,1);in.open={};const auto r=run();EXPECT_EQ(r.neumannComponents,1);EXPECT_EQ(r.dirichletComponents,0);EXPECT_EQ(r.maxClosedNormalFlux,0);EXPECT_EQ(r.maxWallNormalError,0);expectChannel("out.vdb",{0,-1,-2},1);
  in={};channel(3,.25,2,false,2);in.open={};const auto translated=run("translation.vdb");EXPECT_EQ(translated.totalIterations,0);expectChannel("translation.vdb",{0,0,0},8);
  vdb::BoundaryProjectionReplay replay;replay.state=path("translation.vdb");vdb::BoundaryProjectionOptions replayOptions;replayOptions.projection=o.projection;(void)vdb::WriteReprojectedBoundaryState(replay,path("translation-replay.vdb"),replayOptions,p);expectChannel("translation-replay.vdb",{0,0,0},8);
}
TEST_F(OpenVdbBoundaryProjection, IncompatibleClosedMovingWallFluxRejectsWithoutMeanRepairOrOutput){
  channel(1,1,0);in.open={};auto wall=Mac::create(V(0));wall->setName("wall");wall->setIsInWorldSpace(true);for(int x=-2;x<=2;++x)wall->tree().setValue({x,0,0},V(2*x,0,0));write("wall.vdb",{wall});in.wallVelocity={path("wall.vdb"),"wall"};const auto original=bytes(path("wall.vdb"));
  try{(void)run();FAIL();}catch(const std::runtime_error& e){EXPECT_NE(std::string(e.what()).find("incompatible"),std::string::npos);}EXPECT_FALSE(std::filesystem::exists(path("out.vdb")));EXPECT_EQ(original,bytes(path("wall.vdb")));
}
TEST_F(OpenVdbBoundaryProjection, FreeSurfaceHydrostaticsAndEffectiveClampUseTheSamePressureGradient){
  channel(3,.5,0,true);o.airPressure=vdb::AirPressureLocation::LevelSetInterface;o.projection.acceleration={-3,0,0};o.projection.timeStep=.125;
  for(bool clamp:{false,true}){const double theta=clamp?1./1024:.75;auto phi=Scalar::create(3);phi->setName("phi");phi->setTransform(openvdb::math::Transform::createLinearTransform(.5));for(int x=-4;x<=7;++x)phi->tree().setValue({x,0,0},.5*(x-(2+theta)));write(clamp?"clamp-phi.vdb":"phi.vdb",{phi});in.liquidPhi={path(clamp?"clamp-phi.vdb":"phi.vdb"),"phi"};o.minimumInterfaceFraction=clamp?.125:.01;const std::string output=clamp?"clamped.vdb":"hydro.vdb";const auto result=run(output);EXPECT_EQ(result.freeSurfacePressureFaces,1);EXPECT_EQ(result.clampedInterfaceFaces,clamp?1:0);EXPECT_DOUBLE_EQ(result.minimumEffectiveFraction,clamp?.125:.75);expectChannel(output,clamp?std::vector<double>{3.1875,1.6875,.1875}:std::vector<double>{4.125,2.625,1.125},0);if(clamp)EXPECT_DOUBLE_EQ(result.maximumFractionClamp,.125-1./1024);}
}
TEST_F(OpenVdbBoundaryProjection, SevenGridFramesMetadataReplayValidationAndInputBytesRemainExact){
  channel();const auto source=bytes(path("domain.vdb")),wind=bytes(path("wind.vdb"));(void)run();auto grids=all("out.vdb");ASSERT_EQ(grids->size(),7);auto velocity=read<Mac>("out.vdb","velocity");EXPECT_EQ(velocity->getGridClass(),openvdb::GRID_STAGGERED);EXPECT_FALSE(velocity->isInWorldSpace());EXPECT_EQ(velocity->getVectorType(),openvdb::VEC_CONTRAVARIANT_RELATIVE);EXPECT_EQ(velocity->metaValue<std::string>("artist"),"original wind");
  auto pressure=read<Scalar>("out.vdb","pressure");EXPECT_EQ(pressure->metaValue<std::string>("artist"),std::string("domain\0meta",11));openvdb::io::File file(path("out.vdb").string());file.open(false);EXPECT_EQ(file.getMetadata()->metaValue<int64_t>("scene"),INT64_C(9007199254740993));file.close();
  for(auto& grid:*grids)if(grid->getName()=="boundary_fraction"){auto fractions=openvdb::gridPtrCast<Mac>(grid);V value=fractions->tree().getValue({3,0,0});value[0]=1;fractions->tree().setValue({3,0,0},value);}write("bad-state.vdb",*grids);
  vdb::BoundaryProjectionReplay replay;replay.state=path("bad-state.vdb");vdb::BoundaryProjectionOptions controls;controls.projection=o.projection;EXPECT_THROW((void)vdb::WriteReprojectedBoundaryState(replay,path("bad.vdb"),controls,p),std::invalid_argument);EXPECT_FALSE(std::filesystem::exists(path("bad.vdb")));EXPECT_EQ(source,bytes(path("domain.vdb")));EXPECT_EQ(wind,bytes(path("wind.vdb")));
}
TEST_F(OpenVdbBoundaryProjection, SharedCapsInvalidGeometryAndExclusiveUnicodePublicationAreBounded){
  channel();for(int which=0;which<6;++which){auto saved=o;auto limits=p;if(which==0)o.maxWorkItems=1;if(which==1)o.maxMetadataBytes=1;if(which==2)p.maxInputBytes=1024;if(which==3)p.maxVoxelCount=512;if(which==4)o.projection.maxFluidCells=1;if(which==5)o.projection.writeDomain=false;EXPECT_THROW(run(),std::exception)<<which;EXPECT_FALSE(std::filesystem::exists(path("out.vdb")));o=saved;p=limits;}
  auto source=path("domain.vdb");auto unicode=directory/std::filesystem::path(std::u8string(u8"流体-é.vdb"));std::filesystem::copy_file(source,unicode);in.fluid.file=unicode;in.open.file=unicode;const auto output=directory/std::filesystem::path(std::u8string(u8"圧力.vdb"));EXPECT_NO_THROW((void)vdb::WriteBoundaryProjectedVelocity(in,output,o,p));const auto completed=bytes(output);EXPECT_THROW((void)vdb::WriteBoundaryProjectedVelocity(in,output,o,p),std::invalid_argument);EXPECT_EQ(completed,bytes(output));EXPECT_EQ(bytes(source),bytes(unicode));
}
TEST_F(OpenVdbBoundaryProjection, MandatoryDelayedLoadingRejectsBeforeMaterialization){
  channel();struct Restore {
    std::string value;bool present=false;
    static void assign(const char* value) {
#ifdef _WIN32
      _putenv_s("OPENVDB_DISABLE_DELAYED_LOAD",value?value:"");
#else
      if(value)setenv("OPENVDB_DISABLE_DELAYED_LOAD",value,1);
      else unsetenv("OPENVDB_DISABLE_DELAYED_LOAD");
#endif
    }
    Restore(){if(const char* s=std::getenv("OPENVDB_DISABLE_DELAYED_LOAD")){present=true;value=s;}assign("1");}
    ~Restore(){assign(present?value.c_str():nullptr);}
  }restore;
  EXPECT_THROW(run(),std::invalid_argument);EXPECT_FALSE(std::filesystem::exists(path("out.vdb")));
}
TEST_F(OpenVdbBoundaryProjection, CanonicalReplayRejectsForeignFramesActivityPayloadAndBundleTags){
  channel();(void)run();
  for(int which=0;which<10;++which){auto grids=all("out.vdb");
    for(auto& grid:*grids){if(grid->getName()=="velocity"){
        auto u=openvdb::gridPtrCast<Mac>(grid);
        if(which==0)u->setIsInWorldSpace(true);
        if(which==1)u->tree().setValueOn({40,0,0},V(0));
        if(which==2)u->tree().touchLeaf({40,0,0})->setValueOff({40,0,0},V(1,0,0));
        if(which==3){auto v=u->tree().getValue({3,0,0});v[1]=1;u->tree().setValueOn({3,0,0},v);}
        if(which==4)u->removeMeta("container:boundary_projection_state_id");
      }
      if(grid->getName()=="boundary_wall_velocity"&&which==5){grid->setGridClass(openvdb::GRID_UNKNOWN);grid->setIsInWorldSpace(true);}
      if(grid->getName()=="fluid"){
        auto f=openvdb::gridPtrCast<Mask>(grid);
        if(which==6)f->tree().setValueOn({20,0,0},false);
        if(which==7)f->tree().touchLeaf({20,0,0})->setValueOff({20,0,0},true);
      }
      if(grid->getName()=="boundary_fraction"&&which==8)grid->insertMeta("container:boundary_maximum_clamp",openvdb::DoubleMetadata(.1));
      if(which==9){
        if(grid->getName()=="boundary_kind"){auto values=openvdb::gridPtrCast<openvdb::Vec3IGrid>(grid);auto value=values->tree().getValue({3,0,0});value[1]=3;values->tree().setValueOn({3,0,0},value);}
        if(grid->isType<Mac>()){auto values=openvdb::gridPtrCast<Mac>(grid);auto value=values->tree().getValue({3,0,0});value[1]=grid->getName()=="boundary_fraction"?.5:1;values->tree().setValueOn({3,0,0},value);}
      }
    }
    write("bad-state.vdb",*grids);const auto before=bytes(path("bad-state.vdb"));
    vdb::BoundaryProjectionReplay replay;replay.state=path("bad-state.vdb");vdb::BoundaryProjectionOptions controls;controls.projection=o.projection;
    EXPECT_THROW((void)vdb::WriteReprojectedBoundaryState(replay,path("bad.vdb"),controls,p),std::invalid_argument)<<which;
    EXPECT_FALSE(std::filesystem::exists(path("bad.vdb")));EXPECT_EQ(before,bytes(path("bad-state.vdb")));
  }
}
TEST_F(OpenVdbBoundaryProjection, ManufacturedIrregularThreeDimensionalPressureChecksStoredUnpinnedEquations){
  std::set<C> cells;for(int x=-2;x<=2;++x)for(int y=-1;y<=2;++y)for(int z=0;z<=1;++z)if(x!=0||y!=0)cells.insert({x,y,z});
  const auto polynomial=[](const C& c){return c.x()*c.x()+.25*c.x()*c.y()+2.*c.y()*c.y()+.5*c.z();};
  const auto axis=[](int a){C e(0);e[a]=1;return e;};
  for(bool closed:{false,true}){
    auto fluid=Mask::create(false);fluid->setName("fluid");auto u=Mac::create(V(0));u->setName("predictor");u->setGridClass(openvdb::GRID_STAGGERED);u->setIsInWorldSpace(false);u->setVectorType(openvdb::VEC_CONTRAVARIANT_RELATIVE);
    for(const auto& c:cells)fluid->tree().setValueOn(c,true);
    const double gauge=closed?polynomial(*cells.begin()):0;
    const auto pressure=[&](const C& c){return polynomial(c)-gauge;};
    std::set<std::pair<C,int>> faces;for(const auto& c:cells)for(int a=0;a<3;++a){faces.emplace(c,a);faces.emplace(c+axis(a),a);}
    for(const auto& [c,a]:faces){const C left=c-axis(a);const bool l=cells.contains(left),r=cells.contains(c);double predictor=0;
      if(l&&r)predictor=pressure(c)-pressure(left);
      else if(!closed)predictor=2*((r?pressure(c):0)-(l?pressure(left):0));
      auto tuple=u->tree().getValue(c);tuple[a]=predictor;u->tree().setValueOn(c,tuple);
    }
    write("manufactured.vdb",{fluid,u});const auto original=bytes(path("manufactured.vdb"));in={};in.fluid={path("manufactured.vdb"),"fluid"};in.velocity={path("manufactured.vdb"),"predictor"};o.missing=closed?vdb::MissingFluidBoundary::ClosedWall:vdb::MissingFluidBoundary::OpenFacePressure;
    const auto output=closed?"closed.vdb":"open.vdb";const auto report=run(output);EXPECT_EQ(report.projection.componentCount,1);EXPECT_EQ(report.neumannComponents,closed?1:0);EXPECT_EQ(report.dirichletComponents,closed?0:1);EXPECT_GT(report.totalIterations,1);
    const auto storedP=read<Scalar>(output,"pressure");const auto storedU=read<Mac>(output,"velocity");
    for(const auto& c:cells){EXPECT_NEAR(storedP->tree().getValue(c),pressure(c),1e-8)<<c;double residual=0,div=0;
      for(int a=0;a<3;++a){div+=storedU->tree().getValue(c+axis(a))[a]-storedU->tree().getValue(c)[a];
        for(int sign:{-1,1}){const C other=sign>0?c+axis(a):c-axis(a);const double pi=storedP->tree().getValue(c);if(cells.contains(other))residual+=pi-storedP->tree().getValue(other);else if(!closed)residual+=2*pi;}
      }
      double predictorDiv=0;for(int a=0;a<3;++a)predictorDiv+=u->tree().getValue(c+axis(a))[a]-u->tree().getValue(c)[a];
      EXPECT_NEAR(residual+predictorDiv,0,1e-8)<<c;EXPECT_NEAR(div,0,1e-8)<<c;
    }
    for(const auto& [c,a]:faces)EXPECT_NEAR(storedU->tree().getValue(c)[a],0,1e-8);
    EXPECT_EQ(original,bytes(path("manufactured.vdb")));
    auto adequate=o;o.projection.maxIterations=1;o.projection.tolerance=1e-12;
    try{(void)run("not-converged.vdb");FAIL()<<"native IC/PCG must not silently exceed the single-iteration cap";}catch(const std::runtime_error& e){EXPECT_NE(std::string(e.what()).find("original iteration/work caps"),std::string::npos);}
    EXPECT_FALSE(std::filesystem::exists(path("not-converged.vdb")));o=adequate;
  }
}
TEST_F(OpenVdbBoundaryProjection, DisconnectedStrongWeakAndIsolatedClosedComponentsNormalizeIndependently){
  auto fluid=Mask::create(false);fluid->setName("fluid");auto u=Mac::create(V(0));u->setName("u");u->setGridClass(openvdb::GRID_STAGGERED);u->setIsInWorldSpace(false);u->setVectorType(openvdb::VEC_CONTRAVARIANT_RELATIVE);
  for(int row=0;row<2;++row){for(int x=0;x<3;++x)fluid->tree().setValueOn({x,row*8,0},true);for(int x=1;x<3;++x)u->tree().setValueOn({x,row*8,0},V(row?1e-100:1e20,0,0));}fluid->tree().setValueOn({20,0,0},true);
  write("separate.vdb",{fluid,u});in.fluid={path("separate.vdb"),"fluid"};in.velocity={path("separate.vdb"),"u"};const auto r=run();EXPECT_EQ(r.neumannComponents,3);EXPECT_EQ(r.projection.componentCount,3);auto pOut=read<Scalar>("out.vdb","pressure");auto vOut=read<Mac>("out.vdb","velocity");
  for(int row=0;row<2;++row){const double value=row?1e-100:1e20;EXPECT_DOUBLE_EQ(pOut->tree().getValue({0,row*8,0}),0);for(int x=1;x<3;++x){EXPECT_NEAR(pOut->tree().getValue({x,row*8,0})/value,x,1e-9);EXPECT_LE(std::abs(vOut->tree().getValue({x,row*8,0})[0])/value,1e-9);}}
  EXPECT_EQ(pOut->tree().getValue({20,0,0}),0);EXPECT_TRUE(pOut->tree().isValueOn({20,0,0}));
}
TEST_F(OpenVdbBoundaryProjection, FloatFetchPromotionRetainsSubnormalMovingWallNormalBeforeInterpolation){
  channel(1,1,0);auto wall=openvdb::Vec3SGrid::create(openvdb::Vec3s(0));wall->setName("wall");wall->tree().setValueOn({0,0,0},openvdb::Vec3s(std::numeric_limits<float>::denorm_min(),0,0));write("wall.vdb",{wall});in.wallVelocity={path("wall.vdb"),"wall"};const auto r=run();EXPECT_TRUE(r.projection.converged);const double expected=double(std::numeric_limits<float>::denorm_min())*.5;expectChannel("out.vdb",{expected*.5},expected,expected*1e-9);auto stored=read<Mac>("out.vdb","velocity");EXPECT_GT(stored->tree().getValue({0,0,0})[0],0);EXPECT_DOUBLE_EQ(stored->tree().getValue({0,0,0})[0],expected);
}
TEST_F(OpenVdbBoundaryProjection, UnresolvableAmbientGradientAndRemoteOrIllConditionedSamplesFailBeforePublication){
  channel();o.ambientPressure=1e30;try{(void)run();FAIL();}catch(const std::runtime_error& e){EXPECT_NE(std::string(e.what()).find("stored-pressure residual"),std::string::npos);}EXPECT_FALSE(std::filesystem::exists(path("out.vdb")));o.ambientPressure=0;
  auto masks=all("domain.vdb");for(auto& g:*masks){auto t=openvdb::math::Transform::createLinearTransform(1);t->postTranslate(V(1e15,0,0));g->setTransform(t);}write("remote.vdb",*masks);in.fluid.file=in.open.file=path("remote.vdb");EXPECT_THROW(run(),std::invalid_argument);EXPECT_FALSE(std::filesystem::exists(path("out.vdb")));
  in.fluid.file=in.open.file=path("domain.vdb");auto field=Mac::create(V(0));field->setName("wind");openvdb::math::Mat4d matrix;matrix.setIdentity();matrix[0][0]=1;matrix[0][1]=1;matrix[1][0]=1;matrix[1][1]=1+1e-10;field->setTransform(openvdb::math::Transform::createLinearTransform(matrix));write("bad-map.vdb",{field});in.velocity.file=path("bad-map.vdb");EXPECT_THROW(run(),std::invalid_argument);EXPECT_FALSE(std::filesystem::exists(path("out.vdb")));
}
TEST_F(OpenVdbBoundaryProjection, FullTypedPagingRootTraversalAndSummedSevenGridTopologyPreflightBeforeSolve){
  channel();auto velocity=Mac::create(V(0));velocity->setName("wind");for(int x=0;x<80;++x)velocity->tree().setValueOn({16*x,0,0},V(1,0,0));velocity->insertMeta(openvdb::GridBase::META_FILE_MEM_BYTES,openvdb::Int64Metadata(1));velocity->insertMeta(openvdb::GridBase::META_FILE_VOXEL_COUNT,openvdb::Int64Metadata(80));
  {openvdb::io::File f(path("underreported.vdb").string());f.setGridStatsMetadataEnabled(false);f.setCompression(openvdb::io::COMPRESS_ZIP);f.write({velocity});f.close();}
  in.velocity.file=path("underreported.vdb");const auto full=openvdb::tools::memUsageIfLoaded(velocity->tree(),false);p.maxInputBytes=std::filesystem::file_size(path("underreported.vdb"))+3*1024*1024+full;
  try{(void)run();FAIL();}catch(const std::invalid_argument& e){EXPECT_NE(std::string(e.what()).find("before paging"),std::string::npos);}EXPECT_FALSE(std::filesystem::exists(path("out.vdb")));
  p={};in.velocity.file=path("wind.vdb");o.maxWorkItems=512;try{(void)run();FAIL();}catch(const std::invalid_argument& e){EXPECT_NE(std::string(e.what()).find("before paging"),std::string::npos);}o.maxWorkItems=2'000'000'000;
  // Sources fit, but the joint Bool/pressure plus FIVE face grids do not.
  p.maxVoxelCount=2048;EXPECT_THROW(run(),std::invalid_argument);EXPECT_FALSE(std::filesystem::exists(path("out.vdb")));p={};
  auto unused=openvdb::Int32Grid::create(0);unused->setName("unused");unused->insertMeta("large",openvdb::StringMetadata(std::string(6000,'x')));auto domain=all("domain.vdb");domain->push_back(unused);write("large-metadata.vdb",*domain);in.fluid.file=in.open.file=path("large-metadata.vdb");o.maxMetadataBytes=20000;EXPECT_THROW(run(),std::invalid_argument);EXPECT_FALSE(std::filesystem::exists(path("out.vdb")));
}
TEST_F(OpenVdbBoundaryProjection, NativeScaleEndpointsRetainExactMapsPhysicalPressureAndCanonicalReplay){
  for(double h:{1e-6,1e6}){
    auto t=openvdb::math::Transform::createLinearTransform(1);
    if(h==1e-6){std::stringstream record(std::ios::in|std::ios::out|std::ios::binary);openvdb::writeString(record,openvdb::math::UniformScaleMap::mapType());for(double v:{h,h,1/h,(1/h)*(1/h),.5/h})V(v).write(record);record.seekg(0);openvdb::io::setCurrentVersion(record);t->read(record);}else t=openvdb::math::Transform::createLinearTransform(h);
    auto f=Mask::create(false);f->setName("fluid");f->setTransform(t->copy());f->tree().setValueOn({0,0,0},true);f->tree().setValueOn({1,0,0},true);auto outlet=openvdb::MaskGrid::create(false);outlet->setName("open");outlet->setTransform(t->copy());outlet->tree().setValueOn({2,0,0});auto u=Mac::create(V(0));u->setName("u");write("endpoints.vdb",{f,outlet,u});in={};in.fluid={path("endpoints.vdb"),"fluid"};in.open={path("endpoints.vdb"),"open"};in.velocity={path("endpoints.vdb"),"u"};o.wallVelocityWorld={h,0,0};
    const auto name=h<1?"tiny.vdb":"large.vdb";(void)run(name);auto pressure=read<Scalar>(name,"pressure");auto velocity=read<Mac>(name,"velocity");EXPECT_NEAR(pressure->tree().getValue({0,0,0})/(h*h),1.5,1e-9);EXPECT_NEAR(pressure->tree().getValue({1,0,0})/(h*h),.5,1e-9);
    for(int x=0;x<=2;++x)EXPECT_NEAR(velocity->tree().getValue({x,0,0})[0],1,1e-9);
    EXPECT_EQ(velocity->transform().baseMap()->applyJacobian(V(1,0,0)),V(h,0,0));EXPECT_EQ(pressure->transform().baseMap()->applyJacobian(V(0,1,0)),V(0,h,0));
    vdb::BoundaryProjectionReplay replay;replay.state=path(name);vdb::BoundaryProjectionOptions controls;controls.projection=o.projection;const auto again=std::string(name)+"-replay.vdb";(void)vdb::WriteReprojectedBoundaryState(replay,path(again),controls,p);auto repeated=read<Mac>(again,"velocity");for(int x=0;x<=2;++x)EXPECT_NEAR(repeated->tree().getValue({x,0,0})[0],1,1e-9);
  }
}
TEST_F(OpenVdbBoundaryProjection, IndependentShearedWorldFieldsAndInactiveMaskDataKeepDeclaredSemantics){
  channel(3,1,0);auto map=openvdb::math::Transform::createLinearTransform(1);map->postShear(.25,openvdb::math::X_AXIS,openvdb::math::Y_AXIS);map->postTranslate(V(.25,.125,0));
  auto u=Mac::create(V(0)),wall=Mac::create(V(0));u->setName("u");wall->setName("wall");for(auto g:{u,wall}){g->setTransform(map->copy());g->setVectorType(openvdb::VEC_COVARIANT_NORMALIZE);g->setIsInWorldSpace(true);}
  for(int x=-4;x<=7;++x)for(int y=-3;y<=3;++y)for(int z=-3;z<=3;++z){const C c(x,y,z);const V world=map->indexToWorld(c);u->tree().setValueOff(c,V(world[0],0,0));wall->tree().setValueOff(c,V(2+world[1],0,0));}
  write("sheared.vdb",{u,wall});in.velocity={path("sheared.vdb"),"u"};in.wallVelocity={path("sheared.vdb"),"wall"};(void)run();expectChannel("out.vdb",{1.75,.25,-.25},2);
  auto masks=all("domain.vdb");auto f=openvdb::gridPtrCast<Mask>((*masks)[0]);f->tree().touchLeaf({20,0,0})->setValueOff({20,0,0},true);f->tree().setValueOn({21,0,0},false);write("inactive-mask.vdb",*masks);in.fluid.file=in.open.file=path("inactive-mask.vdb");const auto result=run("inactive.vdb");EXPECT_EQ(result.projection.fluidCellCount,3);expectChannel("inactive.vdb",{1.75,.25,-.25},2);
  auto bad=u->deepCopy();bad->tree().touchLeaf({40,0,0})->setValueOff({40,0,0},V(std::numeric_limits<double>::quiet_NaN(),0,0));write("nan.vdb",{bad});in.velocity.file=path("nan.vdb");EXPECT_THROW(run("nan-out.vdb"),std::invalid_argument);EXPECT_FALSE(std::filesystem::exists(path("nan-out.vdb")));
}
template<class Grid>class BoundaryRawTransformFixture final:public Grid {
public:
  explicit BoundaryRawTransformFixture(const Grid& grid)
      :Grid(openvdb::ConstPtrCast<typename Grid::TreeType>(grid.constTreePtr())) {
    this->insertMeta(grid);this->setTransform(std::make_shared<openvdb::math::Transform>(grid.transform()));
  }
  openvdb::GridBase::ConstPtr copyGrid()const override {
    return this->copyReplacingMetadataAndTransform(*this,std::make_shared<openvdb::math::Transform>(this->transform()));
  }
  openvdb::GridBase::Ptr copyGrid()override {
    return openvdb::ConstPtrCast<openvdb::GridBase>(static_cast<const BoundaryRawTransformFixture&>(*this).copyGrid());
  }
};
TEST_F(OpenVdbBoundaryProjection, RawTinyInverseLossRejectsBeforeDirectForceOrCanonicalReplay) {
  for(const double h:{1e6,5e5})for(const double epsilon:{0.,1e-314,-1e-314}) {
    SCOPED_TRACE(h);
    SCOPED_TRACE(epsilon);
    auto matrix=openvdb::math::Mat4d::identity();for(int a=0;a<3;++a)matrix(a,a)=h;
    matrix(0,1)=epsilon;matrix(1,0)=-epsilon;
    std::stringstream record(std::ios::in|std::ios::out|std::ios::binary);
    openvdb::writeString(record,openvdb::math::AffineMap::mapType());matrix.write(record);
    record.seekg(0);openvdb::io::setCurrentVersion(record);
    auto transform=std::make_shared<openvdb::math::Transform>();transform->read(record);
    auto fluid=Mask::create(false);fluid->tree().setValueOn(C(0),true);fluid->setName("fluid");
    auto velocity=Mac::create(V(0)),fraction=Mac::create(V(0)),prescribed=Mac::create(V(0)),wall=Mac::create(V(0));
    auto kind=openvdb::Vec3IGrid::create(openvdb::Vec3i(0));auto pressure=Scalar::create(0);
    velocity->setName("velocity");fraction->setName("boundary_fraction");prescribed->setName("boundary_pressure");
    wall->setName("boundary_wall_velocity");kind->setName("boundary_kind");pressure->setName("pressure");
    openvdb::GridPtrVec grids{fluid,velocity,fraction,prescribed,wall,kind,pressure};
    for(const auto& grid:grids) {
      grid->setTransform(std::make_shared<openvdb::math::Transform>(*transform));
      grid->setIsInWorldSpace(true);grid->setVectorType(openvdb::VEC_INVARIANT);
      grid->insertMeta("container:boundary_projection_schema",openvdb::StringMetadata("negative_xyz_faces_v1"));
      grid->insertMeta("container:boundary_face_channels",openvdb::StringMetadata("negative_faces_xyz"));
      grid->insertMeta("container:boundary_projection_state_id",openvdb::StringMetadata("independent-all-open-inverse-fixture"));
    }
    for(const auto& grid:{velocity,wall}) {
      grid->setGridClass(openvdb::GRID_STAGGERED);grid->setIsInWorldSpace(false);grid->setVectorType(openvdb::VEC_CONTRAVARIANT_RELATIVE);
      grid->insertMeta("container:mac_layout",openvdb::StringMetadata("negative_faces"));
      grid->insertMeta("container:velocity_units",openvdb::StringMetadata("voxels_per_second"));
    }
    for(const auto& c:{C(0),C(1,0,0),C(0,1,0),C(0,0,1)}) {
      openvdb::Vec3i kinds(0);V theta(0);
      for(int a=0;a<3;++a)if(c==C(0)||c[a]==1){kinds[a]=3;theta[a]=.5;}
      kind->tree().setValueOn(c,kinds);fraction->tree().setValueOn(c,theta);
      velocity->tree().setValueOn(c,V(0));wall->tree().setValueOn(c,V(0));prescribed->tree().setValueOn(c,V(0));
    }
    fraction->insertMeta("container:boundary_clamped_faces",openvdb::Int64Metadata(0));
    fraction->insertMeta("container:boundary_maximum_clamp",openvdb::DoubleMetadata(0));
    openvdb::GridPtrVec views;
    views.push_back(std::make_shared<BoundaryRawTransformFixture<Mask>>(*fluid));
    for(const auto& grid:{velocity,fraction,prescribed,wall})views.push_back(std::make_shared<BoundaryRawTransformFixture<Mac>>(*grid));
    views.push_back(std::make_shared<BoundaryRawTransformFixture<openvdb::Vec3IGrid>>(*kind));
    views.push_back(std::make_shared<BoundaryRawTransformFixture<Scalar>>(*pressure));
    write("raw-inverse.vdb",views);const auto before=bytes(path("raw-inverse.vdb"));
    const auto stored=read<Mask>("raw-inverse.vdb","fluid");
    ASSERT_EQ(stored->transform().baseMap()->applyJacobian(V(1,0,0))[1],epsilon);
    ASSERT_EQ(stored->transform().baseMap()->applyJacobian(V(0,1,0))[0],-epsilon);
    // Independent exact Fraction.from_float J^-1*[5e11,0,0]*Double(1e-6).
    // At these h values the rescued local Y components are representable.
    const double expectedY=epsilon==0?0:(epsilon>0?-1:1)*(h==1e6?0x0.00000000003f4p-1022:0x0.0000000000fd0p-1022);
    EXPECT_EQ(expectedY==0,epsilon==0);
    o={};o.projection.timeStep=1e-6;o.projection.acceleration={5e11,0,0};o.maxWorkItems=10'000'000'000ull;
    p.maxInputBytes=2ull<<30;p.maxVoxelCount=128ull<<20;
    in={};in.fluid={path("raw-inverse.vdb"),"fluid"};in.velocity={path("raw-inverse.vdb"),"velocity"};
    vdb::BoundaryProjectionReplay replay;replay.state=path("raw-inverse.vdb");
    for(const bool canonical:{false,true}) {
      const auto output=path(std::string("inverse-")+(canonical?"replay":"direct")+std::to_string(uint64_t(h))+(epsilon>0?"-positive":epsilon<0?"-negative":"-control")+".vdb");
      o.missing=canonical?vdb::MissingFluidBoundary::ClosedWall:vdb::MissingFluidBoundary::OpenFacePressure;
      const auto call=[&]{return canonical?vdb::WriteReprojectedBoundaryState(replay,output,o,p):vdb::WriteBoundaryProjectedVelocity(in,output,o,p);};
      if(epsilon!=0) {
        try{(void)call();FAIL()<<"Unrepresentable inverse coefficient must reject rather than erase a rescuable force";}
        catch(const std::invalid_argument& error){EXPECT_NE(std::string(error.what()).find("cofactor"),std::string::npos);}
        EXPECT_FALSE(std::filesystem::exists(output));
      } else {
        const auto result=call();EXPECT_TRUE(result.projection.converged);EXPECT_EQ(result.openFacePressureFaces,6u);
        EXPECT_EQ(result.projection.maxDivergenceBefore,0);EXPECT_EQ(result.projection.maxDivergenceAfter,0);
        const auto value=read<Mac>(output.filename().string(),"velocity")->tree().getValue(C(0));
        EXPECT_NEAR(value[0],h==1e6?.5:1,2e-15);EXPECT_EQ(value[1],0);EXPECT_EQ(value[2],0);
      }
    }
    EXPECT_EQ(bytes(path("raw-inverse.vdb")),before);
  }
}

TEST_F(OpenVdbBoundaryProjection, WorldVelocityWallAndForceCancellationMatchesIndependentHighPrecisionInverse){
  const double s=std::sqrt(.5),vx=1e12,vy=-std::nextafter(vx,0);
  // Independent Python Decimal100 oracle from the exact stored binary inputs:
  // (vx+vy)/(2*s) = .000086316745750310971199264867534214740910322...
  // Correctly rounded Double, independent of the native frame helper/matrix.
  constexpr double weak=0x1.6a09e667f3bccp-14;
  for(bool reflected:{false,true}){
    openvdb::math::Mat4d matrix;matrix.setIdentity();matrix[0][0]=reflected?-s:s;matrix[0][1]=reflected?-s:s;matrix[1][0]=-s;matrix[1][1]=s;
    auto map=openvdb::math::Transform::createLinearTransform(matrix);auto fluid=Mask::create(false);fluid->setName("fluid");fluid->setTransform(map);fluid->tree().setValueOn({0,0,0},true);
    for(int mode=0;mode<3;++mode){
      auto velocity=Mac::create(mode==0?V(vx,vy,0):V(0));velocity->setName("u");write("cancellation.vdb",{fluid,velocity});in={};in.fluid={path("cancellation.vdb"),"fluid"};in.velocity={path("cancellation.vdb"),"u"};o={};o.projection.timeStep=.125;o.projection.tolerance=1e-10;
      o.missing=mode==1?vdb::MissingFluidBoundary::ClosedWall:vdb::MissingFluidBoundary::OpenFacePressure;
      if(mode==1)o.wallVelocityWorld={vx,vy,0};if(mode==2)o.projection.acceleration={vx,vy,0};
      const auto name=std::to_string(reflected)+"-"+std::to_string(mode)+".vdb";const auto report=run(name);EXPECT_DOUBLE_EQ(report.projection.maxDivergenceBefore,0);EXPECT_DOUBLE_EQ(report.projection.maxDivergenceAfter,0);
      const double expected=(reflected?-1:1)*weak*(mode==2?.125:1);
      auto stored=read<Mac>(name,"velocity");EXPECT_NEAR(stored->tree().getValue({0,0,0})[0],expected,3e-19);EXPECT_NEAR(stored->tree().getValue({1,0,0})[0],expected,3e-19);
      EXPECT_NEAR(stored->tree().getValue({0,0,0})[0]/expected,1,1e-14);
    }
  }
}
#endif
} // namespace

// Independent analytic fixtures supplied by a separate reviewer.
namespace {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
using C = openvdb::Coord;
using V = openvdb::Vec3d;
class BoundaryProjectionAnalyticDraft : public ::testing::Test {
protected:
  struct Channel {
    int n = 4;
    double h = .5;
    openvdb::BoolGrid::Ptr fluid, left, right;
    openvdb::Vec3DGrid::Ptr predictor;
    openvdb::math::Transform::Ptr map;
  };
  void SetUp() override {
    openvdb::initialize();
    static std::atomic_uint64_t serial{};
    directory =
        std::filesystem::temp_directory_path() /
        ("container-boundary-analytic-" +
         std::to_string(
             std::chrono::steady_clock::now().time_since_epoch().count()) +
         "-" + std::to_string(++serial));
    std::filesystem::create_directory(directory);
    options.projection.timeStep = .125;
    options.projection.tolerance = 1e-10;
  }
  void TearDown() override {
    for (const auto &[source, original] : originals)
      EXPECT_EQ(bytes(source), original);
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
  }
  std::filesystem::path path(const std::string &name) const {
    return directory / name;
  }
  static std::string bytes(const std::filesystem::path &file) {
    std::ifstream stream(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), {}};
  }
  void write(const std::string &name, const openvdb::GridPtrVec &grids) const {
    openvdb::MetaMap fileMetadata;
    fileMetadata.insertMeta("fluid_file_note",
                            openvdb::StringMetadata("analytic source"));
    openvdb::io::File output(path(name).string());
    output.setCompression(openvdb::io::COMPRESS_ZIP);
    output.write(grids, fileMetadata);
    output.close();
  }
  template <class G>
  typename G::Ptr read(const std::string &file, const std::string &name) const {
    openvdb::io::File input(path(file).string());
    input.open(false);
    auto grid = openvdb::gridPtrCast<G>(input.readGrid(name));
    input.close();
    return grid;
  }
  openvdb::GridPtrVec readAll(const std::string &file) const {
    openvdb::io::File input(path(file).string());
    input.open(false);
    auto grids = input.getGrids();
    input.close();
    return *grids;
  }
  void protect(const std::string &name) {
    originals[path(name)] = bytes(path(name));
  }
  static C axis(int a) {
    C result(0);
    result[a] = 1;
    return result;
  }
  static double divergence(const openvdb::Vec3DGrid &u, C cell) {
    double value = 0;
    for (int a = 0; a < 3; ++a)
      value +=
          u.tree().getValue(cell + axis(a))[a] - u.tree().getValue(cell)[a];
    return value;
  }
  static void face(openvdb::Vec3DGrid &grid, C coordinate, int a,
                   double value) {
    auto tuple = grid.tree().getValue(coordinate);
    tuple[a] = value;
    grid.tree().setValueOn(coordinate, tuple);
  }
  static openvdb::Vec3DGrid::Ptr mac(const Channel &channel,
                                     const std::string &name) {
    auto grid = openvdb::Vec3DGrid::create(V(0));
    grid->setName(name);
    grid->setTransform(channel.map->copy());
    grid->setGridClass(openvdb::GRID_STAGGERED);
    grid->setIsInWorldSpace(false);
    grid->setVectorType(openvdb::VEC_CONTRAVARIANT_RELATIVE);
    return grid;
  }
  static Channel channel(int count = 4, double h = .5, bool reflected = false) {
    Channel result;
    result.n = count;
    result.h = h;
    result.map = openvdb::math::Transform::createLinearTransform(h);
    result.map->postRotate(.37, openvdb::math::Z_AXIS);
    if (reflected)
      result.map->postScale(V(-1, 1, 1));
    result.map->postTranslate(V(2, -.5, .25));
    const auto mask = [&](const std::string &name) {
      auto grid = openvdb::BoolGrid::create(false);
      grid->setName(name);
      grid->setTransform(result.map->copy());
      return grid;
    };
    result.fluid = mask("source_fluid");
    result.fluid->insertMeta("artist",
                             openvdb::StringMetadata("finite channel"));
    result.left = mask("left");
    result.right = mask("right");
    for (int i = 0; i < count; ++i)
      result.fluid->tree().setValueOn({i, 0, 0}, true);
    result.left->tree().setValueOn({-1, 0, 0}, true);
    result.right->tree().setValueOn({count, 0, 0}, true);
    result.predictor = mac(result, "predictor");
    return result;
  }
  static V worldAxis(const Channel &channel) {
    return channel.map->baseMap()->applyJacobian(V(1, 0, 0)) / channel.h;
  }
  template <class G = openvdb::DoubleGrid, class F>
  static typename G::Ptr scalar(const Channel &channel, const std::string &name,
                                F value) {
    auto grid = G::create(typename G::ValueType(0));
    grid->setName(name);
    grid->setTransform(channel.map->copy());
    grid->setGridClass(openvdb::GRID_UNKNOWN);
    grid->setVectorType(openvdb::VEC_INVARIANT);
    grid->setIsInWorldSpace(true);
    // Sampling positions are on the channel axis; padding ensures a full
    // linear stencil even for effective subcell pressure locations.
    for (int x = -2; x <= channel.n + 2; ++x)
      for (int y = -1; y <= 1; ++y)
        for (int z = -1; z <= 1; ++z)
          grid->tree().setValueOn({x, y, z}, typename G::ValueType(value(x)));
    return grid;
  }
  vdb::BoundaryProjectionInputs inputs(const std::string &source) const {
    vdb::BoundaryProjectionInputs result;
    result.velocity = {path(source), "predictor"};
    result.fluid = {path(source), "source_fluid"};
    return result;
  }
  vdb::BoundaryProjectionResult
  project(const vdb::BoundaryProjectionInputs &input,
          const std::string &output) {
    return vdb::WriteBoundaryProjectedVelocity(input, path(output), options);
  }
  void expectChannel(const std::string &output, const Channel &c, double u,
                     const std::function<double(int)> &p) const {
    auto velocity =
        read<openvdb::Vec3DGrid>(output, options.projection.velocityGridName);
    auto pressure =
        read<openvdb::DoubleGrid>(output, options.projection.pressureGridName);
    EXPECT_EQ(velocity->getGridClass(), openvdb::GRID_STAGGERED);
    EXPECT_EQ(velocity->getVectorType(), openvdb::VEC_CONTRAVARIANT_RELATIVE);
    EXPECT_FALSE(velocity->isInWorldSpace());
    for (int i = 0; i <= c.n; ++i)
      EXPECT_NEAR(velocity->tree().getValue({i, 0, 0}).x(), u, 2e-8);
    for (int i = 0; i < c.n; ++i) {
      EXPECT_NEAR(pressure->tree().getValue({i, 0, 0}), p(i), 2e-8);
      EXPECT_NEAR(divergence(*velocity, {i, 0, 0}), 0, 2e-9);
      for (int a : {1, 2}) {
        EXPECT_NEAR(velocity->tree().getValue({i, 0, 0})[a], 0., 1e-12);
        EXPECT_NEAR(velocity->tree().getValue(C(i, 0, 0) + axis(a))[a], 0.,
                    1e-12);
      }
    }
    EXPECT_EQ(pressure->indexToWorld(V(1, 2, 3)),
              c.map->indexToWorld(V(1, 2, 3)));
  }

  std::filesystem::path directory;
  vdb::BoundaryProjectionOptions options;
  std::map<std::filesystem::path, std::string> originals;
};

TEST_F(BoundaryProjectionAnalyticDraft,
       OuterFaceAndAirCenterPressureHaveDifferentPhysicalDistances) {
  constexpr double localSpeed = 2.;
  for (bool air : {false, true}) {
    auto c = channel(4, .5, true);
    const std::string source = air ? "air-source.vdb" : "open-source.vdb";
    const std::string output = air ? "air.vdb" : "open.vdb";
    write(source, {c.fluid, c.right, c.predictor});
    protect(source);
    auto input = inputs(source);
    if (air)
      input.air = {path(source), "right"};
    else
      input.open = {path(source), "right"};
    const V wall = worldAxis(c) * (localSpeed * c.h);
    options.wallVelocityWorld = {wall.x(), wall.y(), wall.z()};
    const auto result = project(input, output);
    EXPECT_TRUE(result.projection.converged);
    EXPECT_EQ(result.dirichletComponents, 1u);
    EXPECT_EQ(result.neumannComponents, 0u);
    EXPECT_EQ(result.openFacePressureFaces, air ? 0u : 1u);
    EXPECT_EQ(result.airCellPressureFaces, air ? 1u : 0u);
    const double endDistance = air ? 1. : .5;
    const double pressureStep =
        localSpeed * c.h * c.h / options.projection.timeStep;
    expectChannel(output, c, localSpeed, [&](int i) {
      return (c.n - 1 + endDistance - i) * pressureStep;
    });
    auto kind = read<openvdb::Vec3IGrid>(output, options.faceKindGridName);
    auto fraction =
        read<openvdb::Vec3DGrid>(output, options.faceFractionGridName);
    EXPECT_EQ(kind->tree().getValue({c.n, 0, 0}).x(), air ? 4 : 3);
    EXPECT_DOUBLE_EQ(fraction->tree().getValue({c.n, 0, 0}).x(), endDistance);
    EXPECT_EQ(kind->tree().getValue(C(0)).x(), 2);
  }
}

TEST_F(BoundaryProjectionAnalyticDraft,
       NonuniformOutletPressureDrivesAnInitiallyDivergenceFreePredictor) {
  const auto c = channel();
  constexpr double leftPressure = 3., rightPressure = -1.;
  auto pressure = scalar(c, "prescribed", [&](int x) {
    return leftPressure + (rightPressure - leftPressure) * (x + .5) / c.n;
  });
  auto open = c.left->deepCopy();
  open->setName("outlets");
  open->tree().setValueOn({c.n, 0, 0}, true);
  write("source.vdb", {c.fluid, c.predictor, open, pressure});
  protect("source.vdb");
  auto input = inputs("source.vdb");
  input.open = {path("source.vdb"), "outlets"};
  input.pressure = {path("source.vdb"), "prescribed"};
  const auto result = project(input, "driven.vdb");
  EXPECT_DOUBLE_EQ(result.projection.maxDivergenceBefore, 0.);
  EXPECT_TRUE(result.projection.converged);
  EXPECT_GT(result.totalIterations, 0u);
  EXPECT_EQ(result.openFacePressureFaces, 2u);
  const double impulse = options.projection.timeStep / (c.h * c.h) *
                         (leftPressure - rightPressure) / c.n;
  expectChannel("driven.vdb", c, impulse, [&](int i) {
    return leftPressure + (rightPressure - leftPressure) * (i + .5) / c.n;
  });
}

TEST_F(BoundaryProjectionAnalyticDraft,
       BalancedMovingWallsTranslateButIncompatibleNetFluxRejects) {
  const auto c = channel(4, .5, true);
  write("source.vdb", {c.fluid, c.predictor});
  protect("source.vdb");
  constexpr double localSpeed = 2.;
  const V wall = worldAxis(c) * (localSpeed * c.h);
  options.wallVelocityWorld = {wall.x(), wall.y(), wall.z()};
  const auto result = project(inputs("source.vdb"), "translation.vdb");
  EXPECT_EQ(result.neumannComponents, 1u);
  EXPECT_EQ(result.dirichletComponents, 0u);
  EXPECT_NEAR(result.maxClosedNormalFlux, 0., 1e-12);
  EXPECT_NEAR(result.maxWallNormalError, 0., 1e-12);
  expectChannel("translation.vdb", c, localSpeed, [&](int i) {
    return -i * localSpeed * c.h * c.h / options.projection.timeStep;
  });
  auto wallField = mac(c, "wall");
  face(*wallField, {c.n, 0, 0}, 0, localSpeed);
  write("incompatible.vdb", {c.fluid, c.predictor, wallField});
  protect("incompatible.vdb");
  auto input = inputs("incompatible.vdb");
  input.wallVelocity = {path("incompatible.vdb"), "wall"};
  options.wallVelocityWorld = {0, 0, 0};
  EXPECT_THROW((void)project(input, "must-not-exist.vdb"), std::exception);
  EXPECT_FALSE(std::filesystem::exists(path("must-not-exist.vdb")));
}

TEST_F(BoundaryProjectionAnalyticDraft,
       HydrostaticInterfacePressureIsIndependentOfTimestepAndWorldFrame) {
  for (bool reflected : {false, true}) {
    auto c = channel(3, .5, reflected);
    constexpr double theta = .75, gravity = 2.;
    const double surface = c.n - 1 + theta;
    auto phi = scalar(c, "phi", [&](int x) { return c.h * (x - surface); });
    const std::string source = reflected ? "reflected.vdb" : "rotated.vdb";
    write(source, {c.fluid, c.right, c.predictor, phi});
    protect(source);
    auto input = inputs(source);
    input.air = {path(source), "right"};
    input.liquidPhi = {path(source), "phi"};
    options.airPressure = vdb::AirPressureLocation::LevelSetInterface;
    const V acceleration = worldAxis(c) * gravity;
    options.projection.acceleration = {acceleration.x(), acceleration.y(),
                                       acceleration.z()};
    for (double dt : {.125, .25}) {
      options.projection.timeStep = dt;
      const auto output = source + std::to_string(dt) + ".vdb";
      const auto result = project(input, output);
      EXPECT_EQ(result.freeSurfacePressureFaces, 1u);
      EXPECT_EQ(result.clampedInterfaceFaces, 0u);
      EXPECT_NEAR(result.minimumEffectiveFraction, theta, 1e-14);
      expectChannel(output, c, 0.,
                    [&](int i) { return gravity * c.h * (i - surface); });
    }
  }
}

TEST_F(BoundaryProjectionAnalyticDraft,
       InterfaceClampMovesBothThetaAndPressureSamplingLocation) {
  const auto c = channel(3, .5, true);
  constexpr double rawTheta = .005, effectiveTheta = .125, gravity = 2.;
  const double rawSurface = c.n - 1 + rawTheta;
  const double effectiveSurface = c.n - 1 + effectiveTheta;
  auto phi = scalar(c, "phi", [&](int x) { return c.h * (x - rawSurface); });
  // This affine native Float scalar proves promotion and effective sampling;
  // using the old raw phi location would shift every recovered pressure.
  auto prescribed = scalar<openvdb::FloatGrid>(
      c, "prescribed", [](int x) { return 10. + 3. * x; });
  write("source.vdb", {c.fluid, c.right, c.predictor, phi, prescribed});
  protect("source.vdb");
  auto input = inputs("source.vdb");
  input.air = {path("source.vdb"), "right"};
  input.liquidPhi = {path("source.vdb"), "phi"};
  input.pressure = {path("source.vdb"), "prescribed"};
  options.airPressure = vdb::AirPressureLocation::LevelSetInterface;
  options.minimumInterfaceFraction = effectiveTheta;
  const V acceleration = worldAxis(c) * gravity;
  options.projection.acceleration = {acceleration.x(), acceleration.y(),
                                     acceleration.z()};
  const auto result = project(input, "clamped.vdb");
  EXPECT_EQ(result.clampedInterfaceFaces, 1u);
  EXPECT_NEAR(result.minimumEffectiveFraction, effectiveTheta, 1e-14);
  EXPECT_NEAR(result.maximumFractionClamp, effectiveTheta - rawTheta, 1e-13);
  const double pd = 10. + 3. * effectiveSurface;
  expectChannel("clamped.vdb", c, 0., [&](int i) {
    return pd + gravity * c.h * (i - effectiveSurface);
  });
  auto fraction =
      read<openvdb::Vec3DGrid>("clamped.vdb", options.faceFractionGridName);
  auto pressure =
      read<openvdb::Vec3DGrid>("clamped.vdb", options.facePressureGridName);
  EXPECT_NEAR(fraction->tree().getValue({c.n, 0, 0}).x(), effectiveTheta,
              1e-14);
  EXPECT_NEAR(pressure->tree().getValue({c.n, 0, 0}).x(), pd, 1e-12);
}

TEST_F(
    BoundaryProjectionAnalyticDraft,
    ReplayUsesTheSamePredictorAndRepeatingPressureForcingAddsAnotherImpulse) {
  const auto c = channel();
  auto pressure = scalar(c, "prescribed", [&](int x) { return 3. - (x + .5); });
  auto open = c.left->deepCopy();
  open->setName("outlets");
  open->tree().setValueOn({c.n, 0, 0}, true);
  write("source.vdb", {c.fluid, c.predictor, open, pressure});
  protect("source.vdb");
  auto input = inputs("source.vdb");
  input.open = {path("source.vdb"), "outlets"};
  input.pressure = {path("source.vdb"), "prescribed"};
  (void)project(input, "direct.vdb");
  const double impulse = options.projection.timeStep / (c.h * c.h);

  // Preserve all canonical tags, packed tables and incident face activity, but
  // provide the SAME zero predictor used above. Deliberately stale old p is
  // not a warm start or a fixed boundary and must be recomputed.
  auto bundle = readAll("direct.vdb");
  for (auto &grid : bundle) {
    if (grid->getName() == options.projection.velocityGridName) {
      auto velocity = openvdb::gridPtrCast<openvdb::Vec3DGrid>(grid);
      for (auto value = velocity->beginValueAll(); value; ++value)
        value.setValue(V(0));
    } else if (grid->getName() == options.projection.pressureGridName) {
      auto old = openvdb::gridPtrCast<openvdb::DoubleGrid>(grid);
      for (auto value = old->beginValueOn(); value; ++value)
        value.setValue(777.);
    }
  }
  write("same-predictor.vdb", bundle);
  protect("same-predictor.vdb");
  vdb::BoundaryProjectionReplay replay;
  replay.state = path("same-predictor.vdb");
  const auto same =
      vdb::WriteReprojectedBoundaryState(replay, path("same.vdb"), options);
  EXPECT_TRUE(same.replayed);
  expectChannel("same.vdb", c, impulse, [](int i) { return 2.5 - i; });
  auto direct = read<openvdb::Vec3DGrid>("direct.vdb", "velocity");
  auto repeated = read<openvdb::Vec3DGrid>("same.vdb", "velocity");
  for (int i = 0; i <= c.n; ++i)
    EXPECT_EQ(direct->tree().getValue({i, 0, 0}),
              repeated->tree().getValue({i, 0, 0}));

  replay.state = path("direct.vdb");
  const auto second =
      vdb::WriteReprojectedBoundaryState(replay, path("twice.vdb"), options);
  EXPECT_TRUE(second.replayed);
  expectChannel("twice.vdb", c, 2 * impulse, [](int i) { return 2.5 - i; });
}

#endif
} // namespace
