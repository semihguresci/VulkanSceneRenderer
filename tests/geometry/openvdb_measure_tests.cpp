#include "Container/geometry/OpenVdbMeasure.h"
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>
#include <limits>
#include <numbers>

#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
#include <openvdb/openvdb.h>
#include <openvdb/io/File.h>
#include <openvdb/tools/LevelSetSphere.h>
#endif

namespace {
namespace vdb=container::geometry::vdb;
TEST(OpenVdbMeasureAvailability, DisabledBeforePathAndOptions) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  GTEST_SKIP()<<"OpenVDB enabled";
#else
  try { (void)vdb::MeasureLevelSet("missing/no.vdb",{}, {.maxVoxelCount=0,.maxInputBytes=0}); FAIL(); }
  catch (const std::runtime_error& error) { EXPECT_NE(std::string(error.what()).find("ENABLE_OPENVDB=ON"),std::string::npos); }
#endif
}
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
constexpr double Pi=std::numbers::pi;
class OpenVdbMeasure:public ::testing::Test {
 protected:
  void SetUp() override {
    openvdb::initialize(); static std::atomic_uint64_t serial{0};
    directory=std::filesystem::temp_directory_path()/("container-measure-"+
      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+"-"+std::to_string(++serial));
    std::filesystem::create_directory(directory);
  }
  void TearDown() override {
    std::error_code ignored;
    for (const auto& entry:std::filesystem::directory_iterator(directory)) std::filesystem::remove(entry.path(),ignored);
    std::filesystem::remove(directory,ignored);
  }
  auto path(const std::string& name) const { return directory/name; }
  void write(const std::string& name,const openvdb::GridBase::Ptr& grid) {
    if (grid->getName().empty()) grid->setName("shape");
    openvdb::io::File file(path(name).string()); file.write({grid});
  }
  static std::string bytes(const std::filesystem::path& path) {
    std::ifstream in(path,std::ios::binary); return {std::istreambuf_iterator<char>(in),{}};
  }
  static openvdb::FloatGrid::Ptr sphere(double radius=1,double dx=.1) {
    auto grid=openvdb::tools::createLevelSetSphere<openvdb::FloatGrid>(float(radius),{0,0,0},float(dx),3.f);
    grid->setName("shape"); return grid;
  }
  template<class Function> static openvdb::DoubleGrid::Ptr analytic(Function sdf,double extent,double dx) {
    auto grid=openvdb::DoubleGrid::create(3*dx); grid->setName("shape"); grid->setGridClass(openvdb::GRID_LEVEL_SET);
    grid->setTransform(openvdb::math::Transform::createLinearTransform(dx));
    auto acc=grid->getAccessor(); const int n=int(std::ceil(extent/dx));
    for (int x=-n;x<=n;++x) for (int y=-n;y<=n;++y) for (int z=-n;z<=n;++z) {
      const double value=sdf(x*dx,y*dx,z*dx); const openvdb::Coord p(x,y,z);
      if (std::abs(value)<3*dx) acc.setValue(p,value);
      else if (value<0) acc.setValueOff(p,-3*dx);
    }
    return grid;
  }
  std::filesystem::path directory;
};

TEST_F(OpenVdbMeasure, SphereConvergenceAndWorldUnits) {
  write("coarse.vdb",sphere(1,.1)); write("fine.vdb",sphere(1,.05));
  const auto coarse=vdb::MeasureLevelSet(path("coarse.vdb")),fine=vdb::MeasureLevelSet(path("fine.vdb"));
  EXPECT_EQ(fine.gridName,"shape"); EXPECT_GT(fine.activeVoxelCount,coarse.activeVoxelCount);
  EXPECT_NEAR(fine.area,4*Pi,.03); EXPECT_NEAR(fine.volume,4*Pi/3,.02);
  EXPECT_NEAR(fine.averageMeanCurvature,1,.005); EXPECT_NEAR(fine.totalGaussianCurvature,4*Pi,.05);
  EXPECT_EQ(fine.estimatedEulerCharacteristic,2); EXPECT_LT(fine.eulerRoundingError,.01);
  EXPECT_DOUBLE_EQ(fine.eulerEstimate,fine.totalGaussianCurvature/(2*Pi));
  EXPECT_LT(std::abs(fine.area-4*Pi),std::abs(coarse.area-4*Pi));
  EXPECT_LT(std::abs(fine.volume-4*Pi/3),std::abs(coarse.volume-4*Pi/3));
}

TEST_F(OpenVdbMeasure, DoubleTorusHasZeroEulerAndIndependentAreaVolume) {
  constexpr double R=.8,r=.3,dx=.04;
  write("torus.vdb",analytic([](double x,double y,double z){return std::hypot(std::hypot(x,y)-R,z)-r;},1.3,dx));
  const auto result=vdb::MeasureLevelSet(path("torus.vdb"));
  EXPECT_NEAR(result.area,4*Pi*Pi*R*r,.04); EXPECT_NEAR(result.volume,2*Pi*Pi*R*r*r,.02);
  EXPECT_NEAR(result.totalGaussianCurvature,0,.15); EXPECT_EQ(result.estimatedEulerCharacteristic,0);
  EXPECT_GT(result.averageMeanCurvature,0);
}

TEST_F(OpenVdbMeasure, DisjointSpheresHaveAdditiveEulerWithoutGenusInference) {
  write("two.vdb",analytic([](double x,double y,double z) {
    return std::min(std::hypot(x-1.2,y,z),std::hypot(x+1.2,y,z))-.6;
  },2.05,.05));
  const auto result=vdb::MeasureLevelSet(path("two.vdb"));
  EXPECT_NEAR(result.area,8*Pi*.36,.06); EXPECT_NEAR(result.volume,8*Pi*.216/3,.04);
  EXPECT_NEAR(result.averageMeanCurvature,1/.6,.02);
  EXPECT_NEAR(result.totalGaussianCurvature,8*Pi,.2); EXPECT_EQ(result.estimatedEulerCharacteristic,4);
}

TEST_F(OpenVdbMeasure, ScalingChangesAreaVolumeCurvatureWithCorrectPowers) {
  write("one.vdb",sphere(1,.1)); write("two.vdb",sphere(2,.2));
  const auto a=vdb::MeasureLevelSet(path("one.vdb")),b=vdb::MeasureLevelSet(path("two.vdb"));
  EXPECT_NEAR(b.area,4*a.area,1e-10); EXPECT_NEAR(b.volume,8*a.volume,1e-10);
  EXPECT_NEAR(b.averageMeanCurvature,a.averageMeanCurvature/2,1e-12);
  EXPECT_NEAR(b.totalGaussianCurvature,a.totalGaussianCurvature,1e-10);
}

TEST_F(OpenVdbMeasure, LargeWorldAndIndexTranslationsAreConditionedAndSourceUnchanged) {
  auto original=sphere(); write("origin.vdb",original);
  const openvdb::Coord shift(1'000'000'003,-999'999'995,12345);
  auto shifted=openvdb::FloatGrid::create(original->background()); shifted->setName("shape"); shifted->setGridClass(openvdb::GRID_LEVEL_SET);
  shifted->setTransform(original->transform().copy()); shifted->transform().postTranslate({1e15,-1e15,1e15});
  // Exact integer translation of the stored distance field, independent of its
  // world transform, exercises upstream's absolute-index volume cancellation.
  for (auto it=original->cbeginValueAll();it;++it) {
    if (*it==original->background() && !it.isValueOn()) continue;
    auto box=it.getBoundingBox(); box.translate(shift); shifted->tree().fill(box,*it,it.isValueOn());
  }
  shifted->insertMeta("custom",openvdb::StringMetadata("preserved bytes")); write("remote.vdb",shifted);
  const auto before=bytes(path("remote.vdb"));
  const auto a=vdb::MeasureLevelSet(path("origin.vdb")),b=vdb::MeasureLevelSet(path("remote.vdb"));
  EXPECT_NEAR(b.area,a.area,1e-10); EXPECT_NEAR(b.volume,a.volume,1e-10);
  EXPECT_NEAR(b.averageMeanCurvature,a.averageMeanCurvature,1e-12);
  EXPECT_NEAR(b.totalGaussianCurvature,a.totalGaussianCurvature,1e-10);
  EXPECT_EQ(bytes(path("remote.vdb")),before);
}

TEST_F(OpenVdbMeasure, RoundedTopologyAlwaysCarriesRawEstimateAndDiagnostic) {
  // A barely resolved sphere has much stronger integration bias. Topology is
  // still presented as an estimate, whether its integral is near an integer.
  write("small.vdb",sphere(.16,.1)); const auto result=vdb::MeasureLevelSet(path("small.vdb"));
  EXPECT_FALSE(result.topologyDiagnostic.empty());
  EXPECT_DOUBLE_EQ(result.eulerRoundingError,std::abs(result.eulerEstimate-result.estimatedEulerCharacteristic));
  EXPECT_GT(result.eulerRoundingError,.1);
  EXPECT_NE(result.topologyDiagnostic.find("refine"),std::string::npos);
}

TEST_F(OpenVdbMeasure, RejectsNonfiniteAndInvalidBandTopology) {
  auto grid=sphere(); auto first=grid->beginValueOn(); first.setValue(std::numeric_limits<float>::quiet_NaN());
  write("nan.vdb",grid); EXPECT_THROW((void)vdb::MeasureLevelSet(path("nan.vdb")),std::invalid_argument);
  grid=sphere(); grid->tree().setValueOff({0,0,0},-.1f); write("inactive.vdb",grid);
  EXPECT_THROW((void)vdb::MeasureLevelSet(path("inactive.vdb")),std::invalid_argument);
  grid=openvdb::FloatGrid::create(.3f); grid->setGridClass(openvdb::GRID_LEVEL_SET); grid->setTransform(openvdb::math::Transform::createLinearTransform(.1));
  write("empty.vdb",grid); EXPECT_THROW((void)vdb::MeasureLevelSet(path("empty.vdb")),std::invalid_argument);
  grid->tree().setValue({0,0,0},.1f); write("nocross.vdb",grid);
  EXPECT_THROW((void)vdb::MeasureLevelSet(path("nocross.vdb")),std::invalid_argument);
  grid=sphere(); grid->tree().addTile(1,{0,0,0},-.1f,true); write("tile.vdb",grid);
  EXPECT_THROW((void)vdb::MeasureLevelSet(path("tile.vdb")),std::invalid_argument);
  grid=sphere(); grid->tree().root().setBackground(.2f,false); write("band.vdb",grid);
  EXPECT_THROW((void)vdb::MeasureLevelSet(path("band.vdb")),std::invalid_argument);
}

TEST_F(OpenVdbMeasure, RejectsNonuniformTransformsAndUnboundedWorkspace) {
  auto grid=sphere(); grid->transform().postScale({1,2,1}); write("scaled.vdb",grid);
  EXPECT_THROW((void)vdb::MeasureLevelSet(path("scaled.vdb")),std::invalid_argument);
  write("shape.vdb",sphere()); vdb::ProcessingOptions p; p.maxVoxelCount=100;
  EXPECT_THROW((void)vdb::MeasureLevelSet(path("shape.vdb"),{},p),std::invalid_argument);
  p={}; p.maxInputBytes=std::filesystem::file_size(path("shape.vdb"));
  EXPECT_THROW((void)vdb::MeasureLevelSet(path("shape.vdb"),{},p),std::invalid_argument);
  p={}; p.maxInputBytes=6*1024*1024;
  EXPECT_THROW((void)vdb::MeasureLevelSet(path("shape.vdb"),{},p),std::invalid_argument);
  p={}; p.maxVoxelCount=0; EXPECT_THROW((void)vdb::MeasureLevelSet(path("shape.vdb"),{},p),std::invalid_argument);
}

TEST_F(OpenVdbMeasure, IgnoresGenerationSettingsAndHonorsSelectors) {
  auto one=sphere(),two=sphere(2,.2); one->setName("one"); two->setName("two");
  openvdb::io::File file(path("multi.vdb").string()); file.write({one,two});
  vdb::ProcessingOptions p; p.voxelSize=std::numeric_limits<double>::quiet_NaN(); p.halfWidth=0; p.maxTriangles=0; p.gridName.clear();
  const auto result=vdb::MeasureLevelSet(path("multi.vdb"),"two",p); EXPECT_EQ(result.gridName,"two"); EXPECT_NEAR(result.volume,32*Pi/3,.25);
  EXPECT_THROW((void)vdb::MeasureLevelSet(path("multi.vdb"),"absent"),std::invalid_argument);
}

TEST_F(OpenVdbMeasure, RotationReflectionAndTranslationPreserveIntegralUnits) {
  auto grid=sphere(); write("normal.vdb",grid);
  grid->transform().postRotate(.7,openvdb::math::Z_AXIS); grid->transform().postScale({-1,1,1});
  grid->transform().postTranslate({1e6+.125,-2e6+.25,3e6+.5}); write("rotated.vdb",grid);
  const auto a=vdb::MeasureLevelSet(path("normal.vdb")),b=vdb::MeasureLevelSet(path("rotated.vdb"));
  EXPECT_NEAR(a.area,b.area,1e-10); EXPECT_NEAR(a.volume,b.volume,1e-10);
  EXPECT_NEAR(a.averageMeanCurvature,b.averageMeanCurvature,1e-12);
  EXPECT_NEAR(a.totalGaussianCurvature,b.totalGaussianCurvature,1e-10);
}

TEST_F(OpenVdbMeasure, RejectsMissingStatisticsAndCollidingDescriptorsBeforeLookup) {
  auto grid=sphere(); openvdb::io::File noStats(path("nostats.vdb").string());
  noStats.setGridStatsMetadataEnabled(false); noStats.write({grid});
  EXPECT_THROW((void)vdb::MeasureLevelSet(path("nostats.vdb")),std::invalid_argument);
  auto a=sphere(),b=sphere(),c=sphere(); a->setName("field"); b->setName("field"); c->setName("field[0]");
  openvdb::io::File collision(path("collision.vdb").string()); collision.write({a,b,c});
  for (const std::string name:{std::string{},std::string("field[0]")}) {
    try { (void)vdb::MeasureLevelSet(path("collision.vdb"),name); FAIL(); }
    catch (const std::invalid_argument& error) { EXPECT_NE(std::string(error.what()).find("colliding"),std::string::npos); }
  }
}

TEST_F(OpenVdbMeasure, MetadataAndReconstructionAreIncludedInMemoryBudget) {
  auto grid=sphere(); write("plain.vdb",grid);
  vdb::ProcessingOptions p; p.maxInputBytes=32*1024*1024;
  EXPECT_NO_THROW((void)vdb::MeasureLevelSet(path("plain.vdb"),{},p));
  grid->insertMeta("large",openvdb::StringMetadata(std::string(8*1024*1024,'x'))); write("meta.vdb",grid);
  EXPECT_THROW((void)vdb::MeasureLevelSet(path("meta.vdb"),{},p),std::invalid_argument);
  auto boundary=openvdb::DoubleGrid::create(.3); boundary->setGridClass(openvdb::GRID_LEVEL_SET);
  boundary->setTransform(openvdb::math::Transform::createLinearTransform(.1));
  boundary->tree().setValue({std::numeric_limits<int32_t>::max()-2,0,0},-.1);
  write("boundary.vdb",boundary);
  try { (void)vdb::MeasureLevelSet(path("boundary.vdb")); FAIL(); }
  catch (const std::invalid_argument& error) { EXPECT_NE(std::string(error.what()).find("halo"),std::string::npos); }
}
#endif
} // namespace
