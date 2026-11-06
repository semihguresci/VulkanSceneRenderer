#include "Container/geometry/OpenVdbPrimitives.h"
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>

#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
#include <openvdb/openvdb.h>
#include <openvdb/io/File.h>
#include <openvdb/tools/VolumeToMesh.h>
#endif

namespace {
namespace vdb=container::geometry::vdb;
TEST(OpenVdbPrimitivesAvailability, DisabledFailsBeforePathsOrOptions) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  GTEST_SKIP()<<"OpenVDB is enabled";
#else
  try { (void)vdb::WritePlatonicLevelSet("missing-parent/output.vdb",{.faceCount=0}); FAIL(); }
  catch (const std::runtime_error& error) { EXPECT_NE(std::string(error.what()).find("ENABLE_OPENVDB=ON"),std::string::npos); }
#endif
}
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
class OpenVdbPrimitives:public ::testing::Test {
 protected:
  void SetUp() override {
    openvdb::initialize(); static std::atomic_uint64_t serial{0};
    directory=std::filesystem::temp_directory_path()/("container-platonic-"+
      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+"-"+std::to_string(++serial));
    std::filesystem::create_directory(directory); processing.gridName="solid";
  }
  void TearDown() override {
    std::error_code ignored;
    for (const auto& entry:std::filesystem::directory_iterator(directory)) std::filesystem::remove(entry.path(),ignored);
    std::filesystem::remove(directory,ignored);
  }
  auto file(const std::string& name) const { return directory/name; }
  openvdb::FloatGrid::Ptr read(const std::string& name) {
    openvdb::io::File input(file(name).string()); input.open(false);
    auto grid=openvdb::gridPtrCast<openvdb::FloatGrid>(input.readGrid(processing.gridName)); input.close(); return grid;
  }
  static std::string bytes(const std::filesystem::path& path) {
    std::ifstream input(path,std::ios::binary); return {std::istreambuf_iterator<char>(input),{}};
  }
  static double sample(const openvdb::FloatGrid& grid,const openvdb::Vec3d& world) {
    return grid.tree().getValue(grid.transform().worldToIndexCellCentered(world));
  }
  std::filesystem::path directory; vdb::ProcessingOptions processing;
};

TEST_F(OpenVdbPrimitives, AllFiveSolidsRoundTripWithSignedBandAndCanonicalBounds) {
  for (uint32_t faces:{4u,6u,8u,12u,20u}) {
    SCOPED_TRACE(faces); const auto name=std::to_string(faces)+".vdb";
    const auto result=vdb::WritePlatonicLevelSet(file(name),{.faceCount=faces},processing);
    EXPECT_EQ(result.gridName,"solid"); EXPECT_EQ(result.gridClass,"level set"); EXPECT_GT(result.activeVoxelCount,0u);
    EXPECT_DOUBLE_EQ(result.voxelSize,.1);
    const auto grid=read(name);
    EXPECT_EQ(grid->getGridClass(),openvdb::GRID_LEVEL_SET); EXPECT_EQ(grid->voxelSize(),openvdb::Vec3d(.1));
    EXPECT_LT(grid->tree().getValue(openvdb::Coord(0)),0); EXPECT_GT(grid->tree().getValue(openvdb::Coord(30)),0);
    EXPECT_NEAR(grid->background(),.3,1e-7); EXPECT_EQ(grid->metaValue<int32_t>("container:platonic_faces"),faces);
    EXPECT_DOUBLE_EQ(grid->metaValue<double>("container:platonic_scale"),1.);
    EXPECT_EQ(grid->metaValue<openvdb::Vec3d>("container:platonic_center"),openvdb::Vec3d(0));
    bool negative=false,positive=false;
    for (auto value=grid->cbeginValueAll();value;++value) {
      EXPECT_TRUE(std::isfinite(*value)); EXPECT_LE(std::abs(*value),grid->background()+1e-7);
      if (value.isValueOn()) { negative|=*value<0; positive|=*value>0; }
    }
    EXPECT_TRUE(negative); EXPECT_TRUE(positive);
    const auto bounds=grid->evalActiveVoxelBoundingBox();
    const double limit=faces==6?.5:1.;
    for (size_t axis=0;axis<3;++axis) {
      EXPECT_GE(bounds.min()[axis]*.1,-limit-.31); EXPECT_LE(bounds.max()[axis]*.1,limit+.31);
    }
  }
}

TEST_F(OpenVdbPrimitives, IndependentMeshingProducesClosedNondegenerateSurfacesForEverySolid) {
  for (uint32_t faces:{4u,6u,8u,12u,20u}) {
    SCOPED_TRACE(faces); const auto name=std::to_string(faces)+".vdb";
    (void)vdb::WritePlatonicLevelSet(file(name),{.faceCount=faces},processing);
    const auto grid=read(name);
    std::vector<openvdb::Vec3s> points; std::vector<openvdb::Vec3I> triangles; std::vector<openvdb::Vec4I> quads;
    openvdb::tools::volumeToMesh(*grid,points,triangles,quads,0.,0.);
    ASSERT_FALSE(points.empty()); ASSERT_GT(triangles.size()+2*quads.size(),0u);
    std::map<std::pair<uint32_t,uint32_t>,size_t> edges;
    double volume=0;
    const auto triangle=[&](uint32_t a,uint32_t b,uint32_t c) {
      ASSERT_LT(a,points.size()); ASSERT_LT(b,points.size()); ASSERT_LT(c,points.size());
      const openvdb::Vec3d pa(points[a]),pb(points[b]),pc(points[c]);
      EXPECT_GT((pb-pa).cross(pc-pa).lengthSqr(),0);
      volume+=pa.dot(pb.cross(pc))/6;
      for (auto edge:{std::pair{a,b},std::pair{b,c},std::pair{c,a}}) { if (edge.first>edge.second) std::swap(edge.first,edge.second); ++edges[edge]; }
    };
    for (const auto& face:triangles) triangle(face[0],face[1],face[2]);
    for (const auto& face:quads) { triangle(face[0],face[1],face[2]); triangle(face[0],face[2],face[3]); }
    EXPECT_GT(std::abs(volume),.1);
    for (const auto& [edge,count]:edges) EXPECT_EQ(count,2u);
    const double extent=faces==6?.5:1.;
    for (const auto& point:points) for (size_t axis=0;axis<3;++axis) EXPECT_LE(std::abs(point[axis]),extent+.1);
  }
}

TEST_F(OpenVdbPrimitives, CubeDistancesUseWorldUnitsAndPreserveDoubleTranslation) {
  processing.voxelSize=.0625;
  const vdb::PlatonicOptions cube{.faceCount=6,.scale=1.,.center={1e6+.125,-1e6+.123456789,.375}};
  (void)vdb::WritePlatonicLevelSet(file("translated.vdb"),cube,processing);
  const auto grid=read("translated.vdb"); const openvdb::Vec3d center(cube.center[0],cube.center[1],cube.center[2]);
  EXPECT_EQ(grid->indexToWorld(openvdb::Vec3d(0)),center);
  EXPECT_EQ(grid->metaValue<openvdb::Vec3d>("container:platonic_center"),center);
  EXPECT_EQ(grid->transform().baseMap()->applyJacobian(openvdb::Vec3d(1,0,0)),openvdb::Vec3d(.0625,0,0));
  EXPECT_NEAR(sample(*grid,center+openvdb::Vec3d(.625,0,0)),.125,1e-6);
  EXPECT_NEAR(sample(*grid,center+openvdb::Vec3d(.4375,0,0)),-.0625,1e-6);
  EXPECT_NEAR(sample(*grid,center),-.1875,1e-6);
  EXPECT_NEAR(sample(*grid,center+openvdb::Vec3d(.5,0,0)),0,1e-6);
}

TEST_F(OpenVdbPrimitives, OctahedronFaceDistanceAndTetrahedronMinimumInteriorAreResolved) {
  (void)vdb::WritePlatonicLevelSet(file("octa.vdb"),{.faceCount=8},processing);
  const auto octa=read("octa.vdb");
  EXPECT_NEAR(sample(*octa,{.2,.2,.2}),-.4/std::sqrt(3.),.015);
  EXPECT_NEAR(sample(*octa,{.4,.4,.4}),.2/std::sqrt(3.),.015);
  (void)vdb::WritePlatonicLevelSet(file("tetra.vdb"),{.faceCount=4,.scale=.5},processing);
  EXPECT_LT(read("tetra.vdb")->tree().getValue(openvdb::Coord(0)),0);
}

TEST_F(OpenVdbPrimitives, MinimumSupportedVoxelsAndLargeWorldDistancesStayFinite) {
  processing.voxelSize=1.5e-5;
  (void)vdb::WritePlatonicLevelSet(file("small.vdb"),{.faceCount=4,.scale=7.5e-5},processing);
  const auto small=read("small.vdb"); EXPECT_LT(small->tree().getValue(openvdb::Coord(0)),0);
  EXPECT_NEAR(small->background(),4.5e-5,1e-10);
  processing.voxelSize=1e5;
  (void)vdb::WritePlatonicLevelSet(file("large.vdb"),{.faceCount=6,.scale=1e6},processing);
  const auto large=read("large.vdb"); EXPECT_NEAR(sample(*large,{6e5,0,0}),1e5,1);
  EXPECT_TRUE(std::isfinite(large->background())); EXPECT_DOUBLE_EQ(large->voxelSize().x(),1e5);
}

TEST_F(OpenVdbPrimitives, InvalidFacesScaleBandVoxelAndCenterFailBeforeOutput) {
  const auto output=file("bad.vdb");
  for (uint32_t faces:{0u,5u,7u,UINT32_MAX}) EXPECT_THROW((void)vdb::WritePlatonicLevelSet(output,{.faceCount=faces},processing),std::invalid_argument);
  for (double scale:{0.,-1.,1e-7,1e7,std::numeric_limits<double>::quiet_NaN()})
    EXPECT_THROW((void)vdb::WritePlatonicLevelSet(output,{.scale=scale},processing),std::invalid_argument);
  EXPECT_THROW((void)vdb::WritePlatonicLevelSet(output,{.scale=.49},processing),std::invalid_argument);
  for (double band:{2.999,65.,std::numeric_limits<double>::infinity()}) {
    auto p=processing;p.halfWidth=band;EXPECT_THROW((void)vdb::WritePlatonicLevelSet(output,{},p),std::invalid_argument);
  }
  for (double voxel:{0.,1e-6,1e7,std::numeric_limits<double>::quiet_NaN()}) {
    auto p=processing;p.voxelSize=voxel;EXPECT_THROW((void)vdb::WritePlatonicLevelSet(output,{},p),std::invalid_argument);
  }
  EXPECT_THROW((void)vdb::WritePlatonicLevelSet(output,{.center={INFINITY,0,0}},processing),std::invalid_argument);
  EXPECT_FALSE(std::filesystem::exists(output));
}

TEST_F(OpenVdbPrimitives, WorkspaceTopologyTriangleNamesAndPrecisionArePreflighted) {
  const auto output=file("bad.vdb");
  auto p=processing;p.maxVoxelCount=1000;EXPECT_THROW((void)vdb::WritePlatonicLevelSet(output,{},p),std::invalid_argument);
  p=processing;p.maxInputBytes=1024;EXPECT_THROW((void)vdb::WritePlatonicLevelSet(output,{},p),std::invalid_argument);
  p=processing;p.maxTriangles=35;EXPECT_THROW((void)vdb::WritePlatonicLevelSet(output,{.faceCount=12},p),std::invalid_argument);
  p=processing;p.gridName=std::string(257,'x');EXPECT_THROW((void)vdb::WritePlatonicLevelSet(output,{},p),std::invalid_argument);
  EXPECT_THROW((void)vdb::WritePlatonicLevelSet(output,{.center={1e15,0,0}},processing),std::invalid_argument);
  p=processing;p.voxelSize=1.5e-5;
  try { (void)vdb::WritePlatonicLevelSet(output,{.scale=1e6},p); FAIL(); }
  catch (const std::invalid_argument& error) { EXPECT_NE(std::string(error.what()).find("coordinates"),std::string::npos); }
  p=processing;p.voxelSize=.01;
  try { (void)vdb::WritePlatonicLevelSet(output,{.scale=1000},p); FAIL(); }
  catch (const std::invalid_argument& error) { EXPECT_NE(std::string(error.what()).find("precision"),std::string::npos); }
  EXPECT_FALSE(std::filesystem::exists(output));
}

TEST_F(OpenVdbPrimitives, ExistingOutputsAndOptionsArePreservedAndFilesRemainSeekable) {
  std::ofstream(file("protected.vdb"))<<"keep";
  EXPECT_THROW((void)vdb::WritePlatonicLevelSet(file("protected.vdb"),{},processing),std::invalid_argument);
  EXPECT_EQ(bytes(file("protected.vdb")),"keep");
  const vdb::PlatonicOptions primitive{.faceCount=20,.scale=1.,.center={.125,.25,-.375}};
  const auto before=processing;
  (void)vdb::WritePlatonicLevelSet(file("new.vdb"),primitive,processing);
  EXPECT_EQ(primitive.center,(std::array<double,3>{.125,.25,-.375})); EXPECT_EQ(processing.gridName,before.gridName);
  openvdb::io::File input(file("new.vdb").string()); input.open(false);
  const auto metadata=input.readGridMetadata("solid"); EXPECT_EQ(metadata->valueType(),"float");
  EXPECT_TRUE(metadata->getMetadata<openvdb::Int64Metadata>(openvdb::GridBase::META_FILE_MEM_BYTES)); input.close();
  for (const auto& entry:std::filesystem::directory_iterator(directory)) EXPECT_FALSE(entry.is_directory());
}
#endif
} // namespace
