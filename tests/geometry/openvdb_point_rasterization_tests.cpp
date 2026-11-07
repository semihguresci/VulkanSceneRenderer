#include "Container/geometry/OpenVdbPointRasterization.h"
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>
#include <limits>

#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
#include <openvdb/openvdb.h>
#include <openvdb/io/File.h>
#include <openvdb/points/PointDataGrid.h>
#include <openvdb/points/PointAttribute.h>
#endif

namespace {
namespace vdb=container::geometry::vdb;
TEST(OpenVdbPointRasterizationAvailability, DisabledBeforePathsAndOptions) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  GTEST_SKIP()<<"OpenVDB enabled";
#else
  for (int mode:{0,1}) {
    try {
      if (mode==0) (void)vdb::WritePointsLevelSet("missing.vdb","missing/output.vdb",{}, {.voxelSize=0});
      else (void)vdb::WritePointsSplat("missing.vdb","missing/output.vdb",vdb::PointSplatField::Density,{}, {.voxelSize=0});
      FAIL();
    } catch (const std::runtime_error& error) {EXPECT_NE(std::string(error.what()).find("ENABLE_OPENVDB=ON"),std::string::npos);}
  }
#endif
}
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
class OpenVdbPointRasterization:public ::testing::Test {
 protected:
  void SetUp() override {
    openvdb::initialize(); static std::atomic_uint64_t serial{0};
    directory=std::filesystem::temp_directory_path()/("container-point-rasterization-"+
      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+"-"+std::to_string(++serial));
    std::filesystem::create_directory(directory); processing.gridName="field"; processing.voxelSize=1;
  }
  void TearDown() override {
    std::error_code ignored;
    for (const auto& entry:std::filesystem::directory_iterator(directory)) std::filesystem::remove(entry.path(),ignored);
    std::filesystem::remove(directory,ignored);
  }
  auto path(const std::string& name) const {return directory/name;}
  void write(std::span<const vdb::PointRecord> records,const std::string& name="points.vdb") {
    vdb::ProcessingOptions p; p.voxelSize=1; p.gridName="points"; (void)vdb::WritePoints(records,path(name),p);
  }
  template<class Grid> typename Grid::Ptr read(const std::string& file,const std::string& name) {
    openvdb::io::File input(path(file).string()); input.open(false); auto grid=openvdb::gridPtrCast<Grid>(input.readGrid(name)); input.close(); return grid;
  }
  void rewrite(const std::string& file,const openvdb::GridBase::Ptr& grid) {openvdb::io::File output(path(file).string()); output.write({grid});}
  static std::string bytes(const std::filesystem::path& file) {std::ifstream input(file,std::ios::binary); return {std::istreambuf_iterator<char>(input),{}};}
  void noTemporary() {
    for (const auto& entry:std::filesystem::directory_iterator(directory)) EXPECT_TRUE(entry.is_regular_file())<<entry.path();
  }
  std::filesystem::path directory; vdb::ProcessingOptions processing;
};

TEST_F(OpenVdbPointRasterization, SinglePointEightCellHatKernelConservesWorldMass) {
  const std::array<vdb::PointRecord,1> records{{{{.25,.5,.75},{2,3,4},.01}}}; write(records);
  const auto original=bytes(path("points.vdb"));
  const auto result=vdb::WritePointsSplat(path("points.vdb"),path("density.vdb"),vdb::PointSplatField::Density,{},processing,{.massPerPoint=2});
  EXPECT_EQ(result.pointCount,1u); EXPECT_EQ(result.field.activeVoxelCount,8u); EXPECT_TRUE(result.densityGridName.empty());
  EXPECT_DOUBLE_EQ(result.totalMass,2); EXPECT_NEAR(result.depositedMass,2,1e-14);
  const auto grid=read<openvdb::DoubleGrid>("density.vdb","field");
  EXPECT_EQ(grid->getGridClass(),openvdb::GRID_FOG_VOLUME); EXPECT_EQ(grid->background(),0);
  double integral=0;
  for (int x=0;x<2;++x) for (int y=0;y<2;++y) for (int z=0;z<2;++z) {
    const double expected=2*(x?.25:.75)*.5*(z?.75:.25);
    EXPECT_NEAR(grid->tree().getValue({x,y,z}),expected,1e-15); integral+=grid->tree().getValue({x,y,z});
  }
  EXPECT_NEAR(integral,2,1e-14); EXPECT_EQ(bytes(path("points.vdb")),original); noTemporary();
}

TEST_F(OpenVdbPointRasterization, KernelUsesTargetTransformNegativeCoordinatesAndIgnoresRadius) {
  std::array<vdb::PointRecord,1> records{{{{-.25,-.5,-.75},{},.01}}}; write(records,"small.vdb");
  records[0].radius=1000; write(records,"large.vdb");
  (void)vdb::WritePointsSplat(path("small.vdb"),path("a.vdb"),vdb::PointSplatField::Density,{},processing);
  (void)vdb::WritePointsSplat(path("large.vdb"),path("b.vdb"),vdb::PointSplatField::Density,{},processing);
  const auto a=read<openvdb::DoubleGrid>("a.vdb","field"),b=read<openvdb::DoubleGrid>("b.vdb","field");
  EXPECT_EQ(a->activeVoxelCount(),8u); EXPECT_TRUE(a->tree().hasSameTopology(b->tree()));
  for (auto it=a->cbeginValueOn();it;++it) EXPECT_DOUBLE_EQ(*it,b->tree().getValue(it.getCoord()));
  EXPECT_NEAR(a->tree().getValue({0,0,0}),.75*.5*.25,1e-15);
  EXPECT_NE(a->metaValue<std::string>("container:splat_kernel").find("independent"),std::string::npos);
}

TEST_F(OpenVdbPointRasterization, AttributeMeansAreNormalizedAndCompanionDensityMatches) {
  const std::array<vdb::PointRecord,2> records{{{{.25,.5,.75},{2,4,6},.2},{{.25,.5,.75},{4,8,12},.6}}}; write(records);
  for (const auto field:{vdb::PointSplatField::Velocity,vdb::PointSplatField::Radius}) {
    const auto name=field==vdb::PointSplatField::Velocity?"velocity.vdb":"radius.vdb";
    const auto result=vdb::WritePointsSplat(path("points.vdb"),path(name),field,{},processing,{.massPerPoint=3});
    EXPECT_EQ(result.densityGridName,"point_density"); EXPECT_NEAR(result.totalMass,6,1e-14);
    const auto density=read<openvdb::DoubleGrid>(name,"point_density");
    if (field==vdb::PointSplatField::Velocity) {
      const auto grid=read<openvdb::Vec3DGrid>(name,"field");
      EXPECT_TRUE(grid->isInWorldSpace()); EXPECT_EQ(grid->getVectorType(),openvdb::VEC_CONTRAVARIANT_RELATIVE);
      EXPECT_TRUE(grid->tree().hasSameTopology(density->tree()));
      for (auto it=grid->cbeginValueOn();it;++it) EXPECT_EQ(*it,openvdb::Vec3d(3,6,9));
    } else {
      const auto grid=read<openvdb::DoubleGrid>(name,"field"); EXPECT_TRUE(grid->tree().hasSameTopology(density->tree()));
      for (auto it=grid->cbeginValueOn();it;++it) EXPECT_NEAR(*it,.4,1e-15);
    }
    double mass=0; for (auto it=density->cbeginValueOn();it;++it) mass+=*it; EXPECT_NEAR(mass,6,1e-14);
  }
}

TEST_F(OpenVdbPointRasterization, ExtremeOppositeVelocitiesAndTinyMassFractionsRemainStable) {
  const double maximum=std::numeric_limits<double>::max();
  std::array<vdb::PointRecord,2> records{{{{0,0,0},{maximum,0,0},1},{{0,0,0},{-maximum,0,0},1}}}; write(records,"opposite.vdb");
  (void)vdb::WritePointsSplat(path("opposite.vdb"),path("mean.vdb"),vdb::PointSplatField::Velocity,{},processing);
  EXPECT_EQ(read<openvdb::Vec3DGrid>("mean.vdb","field")->tree().getValue({0,0,0}),openvdb::Vec3d(0));
  records={{{{1e-20,0,0},{maximum,0,0},1},{{1,0,0},{0,0,0},1}}}; write(records,"tiny.vdb");
  const auto decoded=vdb::ReadPoints(path("tiny.vdb"));
  (void)vdb::WritePointsSplat(path("tiny.vdb"),path("tiny-mean.vdb"),vdb::PointSplatField::Velocity,{},processing);
  const auto value=read<openvdb::Vec3DGrid>("tiny-mean.vdb","field")->tree().getValue({1,0,0}).x();
  EXPECT_TRUE(std::isfinite(value)); EXPECT_GT(value,1e287); EXPECT_NEAR(value/(maximum*decoded[0].position[0]),1,1e-14);
}

TEST_F(OpenVdbPointRasterization, VoxelVolumeScalingPreservesMassAtLargeAndSmallVoxelSizes) {
  for (double dx:{1.5e-5,1e6}) {
    const std::array<vdb::PointRecord,1> records{{{{dx*.25,dx*.5,dx*.75},{},1}}};
    const auto source=dx<1?"small.vdb":"large.vdb",output=dx<1?"small-density.vdb":"large-density.vdb";
    write(records,source); auto p=processing; p.voxelSize=dx;
    const auto result=vdb::WritePointsSplat(path(source),path(output),vdb::PointSplatField::Density,{},p,{.massPerPoint=2});
    EXPECT_NEAR(result.depositedMass,2,1e-13);
    const auto grid=read<openvdb::DoubleGrid>(output,"field"); double integral=0;
    for (auto it=grid->cbeginValueOn();it;++it) integral+=*it*dx*dx*dx;
    EXPECT_NEAR(integral,2,1e-13);
  }
}

TEST_F(OpenVdbPointRasterization, PerPointRadiiCreateSdfWithSignOverlapAndMetadataPreservation) {
  const std::array<vdb::PointRecord,2> records{{{{-.35,0,0},{},.5},{{.35,0,0},{},.4}}}; write(records);
  auto source=read<openvdb::points::PointDataGrid>("points.vdb","points");
  source->insertMeta("custom:units",openvdb::StringMetadata("meters")); rewrite("points.vdb",source);
  const auto original=bytes(path("points.vdb")); auto p=processing; p.voxelSize=.1;
  const auto result=vdb::WritePointsLevelSet(path("points.vdb"),path("sdf.vdb"),{},p);
  EXPECT_EQ(result.gridClass,"level set"); const auto grid=read<openvdb::FloatGrid>("sdf.vdb","field");
  EXPECT_EQ(grid->getGridClass(),openvdb::GRID_LEVEL_SET); EXPECT_LT(grid->tree().getValue({0,0,0}),0);
  EXPECT_LT(grid->tree().getValue({-3,0,0}),-.2); EXPECT_GT(grid->tree().getValue({10,0,0}),0);
  EXPECT_NEAR(grid->tree().getValue({-10,0,0}),.15,2e-6);
  EXPECT_EQ(grid->metaValue<std::string>("custom:units"),"meters"); EXPECT_EQ(grid->metaValue<int64_t>("container:source_point_count"),2);
  EXPECT_EQ(bytes(path("points.vdb")),original); noTemporary();
}

TEST_F(OpenVdbPointRasterization, EmptySplatAndSdfCutoffDiagnosticsAreExplicit) {
  write({}); const auto result=vdb::WritePointsSplat(path("points.vdb"),path("empty.vdb"),vdb::PointSplatField::Velocity,{},processing);
  EXPECT_EQ(result.pointCount,0u); EXPECT_EQ(result.totalMass,0); EXPECT_EQ(result.depositedMass,0);
  EXPECT_EQ(read<openvdb::DoubleGrid>("empty.vdb","point_density")->activeVoxelCount(),0u);
  EXPECT_THROW((void)vdb::WritePointsLevelSet(path("points.vdb"),path("empty-sdf.vdb"),{},processing),std::invalid_argument);
  const std::array<vdb::PointRecord,1> tiny{{{{0,0,0},{},.1}}}; write(tiny,"tiny.vdb");
  try {(void)vdb::WritePointsLevelSet(path("tiny.vdb"),path("tiny-sdf.vdb"),{},processing); FAIL();}
  catch (const std::invalid_argument& error) {EXPECT_NE(std::string(error.what()).find("[1.5, 100]"),std::string::npos);}
  EXPECT_FALSE(std::filesystem::exists(path("tiny-sdf.vdb"))); noTemporary();
}

TEST_F(OpenVdbPointRasterization, NumericOverflowAndPositiveUnderflowRejectWithoutPartialOutputs) {
  const std::array<vdb::PointRecord,1> point{{{{.25,.5,.75},{},1}}}; write(point);
  const std::array<vdb::PointRecord,1> nodal{{{{0,0,0},{},1}}}; write(nodal,"nodal.vdb");
  auto p=processing; p.voxelSize=1e6;
  try {(void)vdb::WritePointsSplat(path("nodal.vdb"),path("under.vdb"),vdb::PointSplatField::Density,{},p,{.massPerPoint=1e-320}); FAIL();}
  catch (const std::invalid_argument& error) {EXPECT_NE(std::string(error.what()).find("voxel-volume scaling"),std::string::npos);}
  p.voxelSize=1.5e-5;
  EXPECT_THROW((void)vdb::WritePointsSplat(path("points.vdb"),path("over.vdb"),vdb::PointSplatField::Density,{},p,{.massPerPoint=1e308}),std::invalid_argument);
  EXPECT_THROW((void)vdb::WritePointsSplat(path("points.vdb"),path("kernel.vdb"),vdb::PointSplatField::Density,{},processing,{.massPerPoint=std::numeric_limits<double>::denorm_min()}),std::invalid_argument);
  EXPECT_FALSE(std::filesystem::exists(path("under.vdb"))); EXPECT_FALSE(std::filesystem::exists(path("over.vdb")));
  EXPECT_FALSE(std::filesystem::exists(path("kernel.vdb"))); noTemporary();
}

TEST_F(OpenVdbPointRasterization, RepeatedTransfersAreDeterministicAndRadiusCanReachDoubleMaximum) {
  const double maximum=std::numeric_limits<double>::max();
  const std::array<vdb::PointRecord,2> point{{{{.25,.5,.75},{},maximum},{{.25,.5,.75},{},maximum}}}; write(point);
  for (const std::string name:{"a.vdb","b.vdb"})
    (void)vdb::WritePointsSplat(path("points.vdb"),path(name),vdb::PointSplatField::Radius,{},processing);
  const auto a=read<openvdb::DoubleGrid>("a.vdb","field"),b=read<openvdb::DoubleGrid>("b.vdb","field");
  EXPECT_TRUE(a->tree().hasSameTopology(b->tree()));
  for (auto it=a->cbeginValueOn();it;++it) {EXPECT_EQ(*it,maximum); EXPECT_EQ(*it,b->tree().getValue(it.getCoord()));}
  EXPECT_THROW((void)vdb::WritePointsSplat(path("points.vdb"),path("overflow.vdb"),vdb::PointSplatField::Density,{},processing,{.massPerPoint=maximum}),std::invalid_argument);
  EXPECT_FALSE(std::filesystem::exists(path("overflow.vdb"))); noTemporary();
}

TEST_F(OpenVdbPointRasterization, StrictAttributesSelectorsAndCustomMetadataComeFromSameSnapshot) {
  const std::array<vdb::PointRecord,1> one{{{{0,0,0},{},1}}},two{{{{1,0,0},{},1}}}; write(one,"a.vdb"); write(two,"b.vdb");
  auto a=read<openvdb::points::PointDataGrid>("a.vdb","points"),b=read<openvdb::points::PointDataGrid>("b.vdb","points");
  a->insertMeta("custom",openvdb::StringMetadata("a")); b->insertMeta("custom",openvdb::StringMetadata("b"));
  {openvdb::io::File output(path("multi.vdb").string()); output.write({a,b});}
  EXPECT_THROW((void)vdb::WritePointsSplat(path("multi.vdb"),path("ambiguous.vdb"),vdb::PointSplatField::Density,"points",processing),std::invalid_argument);
  (void)vdb::WritePointsSplat(path("multi.vdb"),path("selected.vdb"),vdb::PointSplatField::Radius,"descriptor:points[1]",processing);
  const auto selected=read<openvdb::DoubleGrid>("selected.vdb","field"); EXPECT_EQ(selected->metaValue<std::string>("custom"),"b");
  EXPECT_TRUE(selected->tree().isValueOn({1,0,0})); EXPECT_FALSE(selected->tree().isValueOn({0,0,0}));
  openvdb::points::appendAttribute<float>(a->tree(),"extra",1); rewrite("extra.vdb",a);
  EXPECT_THROW((void)vdb::WritePointsSplat(path("extra.vdb"),path("bad.vdb"),vdb::PointSplatField::Density,{},processing),std::invalid_argument); noTemporary();
}

TEST_F(OpenVdbPointRasterization, AggregateBudgetsMetadataAndOutputProtectionAreEnforced) {
  const std::array<vdb::PointRecord,1> point{{{{.25,.5,.75},{},1}}}; write(point);
  auto p=processing; p.maxVoxelCount=8;
  EXPECT_THROW((void)vdb::WritePointsSplat(path("points.vdb"),path("voxels.vdb"),vdb::PointSplatField::Velocity,{},p),std::invalid_argument);
  p=processing; p.maxInputBytes=8*1024*1024;
  EXPECT_THROW((void)vdb::WritePointsSplat(path("points.vdb"),path("memory.vdb"),vdb::PointSplatField::Density,{},p),std::invalid_argument);
  EXPECT_THROW((void)vdb::WritePointsSplat(path("points.vdb"),path("count.vdb"),vdb::PointSplatField::Density,{},processing,{}, {.maxPoints=0}),std::invalid_argument);
  EXPECT_THROW((void)vdb::WritePointsSplat(path("points.vdb"),path("name.vdb"),vdb::PointSplatField::Radius,{},processing,{.densityGridName="field"}),std::invalid_argument);
  {std::ofstream output(path("existing.vdb")); output<<"preserve";} const auto before=bytes(path("existing.vdb"));
  EXPECT_THROW((void)vdb::WritePointsSplat(path("points.vdb"),path("existing.vdb"),vdb::PointSplatField::Density,{},processing),std::invalid_argument);
  EXPECT_EQ(bytes(path("existing.vdb")),before);
  auto source=read<openvdb::points::PointDataGrid>("points.vdb","points");
  source->insertMeta("large",openvdb::StringMetadata(std::string(8*1024*1024,'x'))); rewrite("metadata.vdb",source);
  p=processing; p.maxInputBytes=128*1024*1024;
  EXPECT_THROW((void)vdb::WritePointsSplat(path("metadata.vdb"),path("large.vdb"),vdb::PointSplatField::Velocity,{},p),std::invalid_argument);
  noTemporary();
}
#endif
} // namespace
