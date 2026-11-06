#include "Container/geometry/OpenVdbMeshConversion.h"
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <fstream>
#include <limits>

#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
#include "Container/geometry/DotBimLoader.h"
#include <openvdb/openvdb.h>
#include <openvdb/io/File.h>
#endif

namespace {
namespace vdb=container::geometry::vdb;
using Position=std::array<double,3>;
using Triangle=std::array<uint32_t,3>;
TEST(OpenVdbMeshConversionAvailability, DisabledBeforeInputPathAndOptions) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  GTEST_SKIP()<<"OpenVDB enabled";
#else
  try { (void)vdb::WriteMeshDistance(std::span<const Position>{},std::span<const Triangle>{},"missing/output.vdb",{.voxelSize=0}); FAIL(); }
  catch (const std::runtime_error& error) { EXPECT_NE(std::string(error.what()).find("ENABLE_OPENVDB=ON"),std::string::npos); }
#endif
}
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
class OpenVdbMeshConversion:public ::testing::Test {
 protected:
  void SetUp() override {
    openvdb::initialize(); static std::atomic_uint64_t serial{0};
    directory=std::filesystem::temp_directory_path()/("container-mesh-distance-"+
      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+"-"+std::to_string(++serial));
    std::filesystem::create_directory(directory); processing.gridName="distance";
  }
  void TearDown() override {
    std::error_code ignored; for (const auto& entry:std::filesystem::directory_iterator(directory)) std::filesystem::remove(entry.path(),ignored);
    std::filesystem::remove(directory,ignored);
  }
  auto path(const std::string& name) const {return directory/name;}
  template<class Grid> typename Grid::Ptr read(const std::string& file,const std::string& name) {
    openvdb::io::File input(path(file).string()); input.open(false); auto grid=openvdb::gridPtrCast<Grid>(input.readGrid(name)); input.close(); return grid;
  }
  static std::string bytes(const std::filesystem::path& path) {std::ifstream in(path,std::ios::binary); return {std::istreambuf_iterator<char>(in),{}};}
  static auto point(const openvdb::GridBase& grid,double x,double y,double z) {return grid.transform().worldToIndexCellCentered({x,y,z});}
  std::array<Position,4> square{{{-1,-1,0},{1,-1,0},{1,1,0},{-1,1,0}}};
  std::array<Triangle,2> triangles{{{0,1,2},{0,2,3}}};
  std::filesystem::path directory; vdb::ProcessingOptions processing;
};

TEST_F(OpenVdbMeshConversion, OpenPlaneHasUnsignedAnalyticDistancesAndNativeTriangleIds) {
  const auto result=vdb::WriteMeshDistance(square,triangles,path("plane.vdb"),processing);
  EXPECT_EQ(result.distance.gridName,"distance"); EXPECT_EQ(result.distance.gridClass,"unsigned distance");
  EXPECT_EQ(result.triangleGridName,"triangle_id"); EXPECT_EQ(result.triangleCount,2u);
  auto distance=read<openvdb::FloatGrid>("plane.vdb","distance"); auto ids=read<openvdb::Int32Grid>("plane.vdb","triangle_id");
  EXPECT_EQ(distance->getGridClass(),openvdb::GRID_UNKNOWN); EXPECT_EQ(ids->getGridClass(),openvdb::GRID_UNKNOWN);
  EXPECT_NEAR(distance->background(),.3,1e-7); EXPECT_EQ(ids->background(),-1);
  EXPECT_EQ(distance->transform(),ids->transform()); EXPECT_EQ(distance->activeVoxelCount(),ids->activeVoxelCount());
  EXPECT_TRUE(distance->tree().hasSameTopology(ids->tree()));
  EXPECT_NEAR(distance->tree().getValue(point(*distance,0,0,.2)),.2,1e-6);
  EXPECT_NEAR(distance->tree().getValue(point(*distance,1.1,0,.1)),std::sqrt(.02),1e-6);
  EXPECT_NEAR(distance->tree().getValue(point(*distance,1.1,1.1,.1)),std::sqrt(.03),1e-6);
  EXPECT_EQ(ids->tree().getValue(point(*ids,.8,-.8,.2)),0);
  EXPECT_EQ(ids->tree().getValue(point(*ids,-.8,.8,.2)),1);
  EXPECT_EQ(ids->tree().getValue(point(*ids,0,0,.2)),0);
  EXPECT_EQ(ids->tree().getValue(point(*ids,0,0,1)),-1);
  for (auto it=distance->cbeginValueOn();it;++it) {
    const auto world=distance->indexToWorld(it.getCoord());
    const double dx=std::max(std::abs(world.x())-1.,0.),dy=std::max(std::abs(world.y())-1.,0.);
    EXPECT_NEAR(*it,std::hypot(dx,dy,world.z()),1e-6);
    EXPECT_GE(*it,0); EXPECT_TRUE(ids->tree().isValueOn(it.getCoord()));
    const auto id=ids->tree().getValue(it.getCoord()); EXPECT_GE(id,0); EXPECT_LT(id,2);
  }
}

TEST_F(OpenVdbMeshConversion, DuplicateTriangleTiesChooseLowestIdDeterministically) {
  const std::array<Triangle,3> duplicates{{{0,1,2},{0,1,2},{2,1,0}}};
  for (const std::string name:{"one.vdb","two.vdb"}) {
    (void)vdb::WriteMeshDistance(square,duplicates,path(name),processing);
    const auto ids=read<openvdb::Int32Grid>(name,"triangle_id");
    for (auto it=ids->cbeginValueOn();it;++it) EXPECT_EQ(*it,0);
  }
  const auto a=read<openvdb::FloatGrid>("one.vdb","distance"),b=read<openvdb::FloatGrid>("two.vdb","distance");
  EXPECT_TRUE(a->tree().hasSameTopology(b->tree()));
  for (auto it=a->cbeginValueOn();it;++it) EXPECT_EQ(*it,b->tree().getValue(it.getCoord()));
}

TEST_F(OpenVdbMeshConversion, WorldTranslationPreservesDoublePlacementAndDistanceUnits) {
  auto translated=square; const Position center{1e6+.125,-1e6+.25,1e6+.375};
  for (auto& p:translated) for (int axis=0;axis<3;++axis) p[axis]+=center[axis];
  (void)vdb::WriteMeshDistance(translated,triangles,path("remote.vdb"),processing);
  const auto grid=read<openvdb::FloatGrid>("remote.vdb","distance");
  const auto origin=grid->indexToWorld(openvdb::Vec3d(0));
  for (int axis=0;axis<3;++axis) EXPECT_DOUBLE_EQ(origin[axis],center[axis]);
  EXPECT_NEAR(grid->tree().getValue(point(*grid,center[0],center[1],center[2]+.2)),.2,1e-6);
  EXPECT_NEAR(grid->tree().getValue(point(*grid,center[0],center[1],center[2]-.2)),.2,1e-6);
}

TEST_F(OpenVdbMeshConversion, DisconnectedParallelSheetsChooseNearestSurfaceAndSymmetricTies) {
  std::vector<Position> points; std::vector<Triangle> faces;
  for (double z:{-.2,.2}) {
    const auto base=uint32_t(points.size());
    for (const auto& point:square) points.push_back({point[0],point[1],z});
    for (const auto& triangle:triangles) faces.push_back({base+triangle[0],base+triangle[1],base+triangle[2]});
  }
  const auto beforePoints=points;
  const auto beforeFaces=faces;
  (void)vdb::WriteMeshDistance(points,faces,path("sheets.vdb"),processing);
  const auto grid=read<openvdb::FloatGrid>("sheets.vdb","distance");
  const auto ids=read<openvdb::Int32Grid>("sheets.vdb","triangle_id");
  for (double z:{-.1,0.,.1}) {
    const auto p=point(*grid,.5,-.5,z);
    EXPECT_NEAR(grid->tree().getValue(p),std::min(std::abs(z-.2),std::abs(z+.2)),1e-6);
    EXPECT_EQ(ids->tree().getValue(p),z<=0?0:2);
  }
  EXPECT_EQ(points,beforePoints); EXPECT_EQ(faces,beforeFaces);
  EXPECT_EQ(grid->metaValue<int64_t>("container:triangle_count"),4);
}

TEST_F(OpenVdbMeshConversion, ClosedMeshInteriorRemainsUnsignedAndClamped) {
  const std::array<Position,8> points{{{-1,-1,-1},{1,-1,-1},{1,1,-1},{-1,1,-1},
    {-1,-1,1},{1,-1,1},{1,1,1},{-1,1,1}}};
  const std::array<Triangle,12> faces{{{0,1,2},{0,2,3},{4,5,6},{4,6,7},
    {0,1,5},{0,5,4},{1,2,6},{1,6,5},{2,3,7},{2,7,6},{3,0,4},{3,4,7}}};
  (void)vdb::WriteMeshDistance(points,faces,path("cube.vdb"),processing);
  const auto grid=read<openvdb::FloatGrid>("cube.vdb","distance");
  const auto ids=read<openvdb::Int32Grid>("cube.vdb","triangle_id");
  EXPECT_NEAR(grid->tree().getValue({0,0,0}),.3,1e-7); EXPECT_EQ(ids->tree().getValue({0,0,0}),-1);
  EXPECT_NEAR(grid->tree().getValue(point(*grid,.8,0,0)),.2,1e-6);
  EXPECT_NEAR(grid->tree().getValue(point(*grid,1.2,0,0)),.2,1e-6);
  EXPECT_EQ(grid->getGridClass(),openvdb::GRID_UNKNOWN);
}

TEST_F(OpenVdbMeshConversion, IdGridCanBeOmittedAndSignedModeIsExplicitlyRejected) {
  vdb::MeshConversionOptions c; c.writeTriangleIds=false;
  const auto result=vdb::WriteMeshDistance(square,triangles,path("distance.vdb"),processing,c);
  EXPECT_TRUE(result.triangleGridName.empty()); openvdb::io::File input(path("distance.vdb").string()); input.open(false);
  size_t grids=0; for (auto it=input.beginName();it!=input.endName();++it) ++grids; EXPECT_EQ(grids,1u); input.close();
  c.unsignedDistance=false; EXPECT_THROW((void)vdb::WriteMeshDistance(square,triangles,path("signed.vdb"),processing,c),std::invalid_argument);
  EXPECT_FALSE(std::filesystem::exists(path("signed.vdb")));
}

TEST_F(OpenVdbMeshConversion, TransformedModelInstancesRetainStableTriangleProvenance) {
  namespace dotbim=container::geometry::dotbim;
  dotbim::Model model; model.vertices.resize(3);
  model.vertices[0].position={-.5f,-.5f,0}; model.vertices[1].position={.5f,-.5f,0}; model.vertices[2].position={0,.5f,0};
  model.indices={0,1,2}; model.meshRanges.push_back({.meshId=42,.firstIndex=0,.indexCount=3});
  dotbim::Element left; left.meshId=42; left.transform=glm::translate(glm::mat4(1),glm::vec3(-1,0,0));
  dotbim::Element right=left; right.transform=glm::translate(glm::mat4(1),glm::vec3(1,0,0))*glm::scale(glm::mat4(1),glm::vec3(1,2,1));
  model.elements={left,right}; const auto sourceVertices=model.vertices; const auto sourceIndices=model.indices;
  const auto result=vdb::WriteMeshDistance(model,path("instances.vdb"),processing); EXPECT_EQ(result.triangleCount,2u);
  const auto grid=read<openvdb::FloatGrid>("instances.vdb","distance");
  const auto ids=read<openvdb::Int32Grid>("instances.vdb","triangle_id");
  EXPECT_EQ(ids->tree().getValue(point(*ids,-1,0,.1)),0); EXPECT_EQ(ids->tree().getValue(point(*ids,1,0,.1)),1);
  EXPECT_NEAR(grid->tree().getValue(point(*grid,1,.5,.1)),.1,1e-6);
  EXPECT_EQ(ids->metaValue<std::string>("container:triangle_provenance"),"[[0,42,0],[1,42,0]]");
  EXPECT_EQ(grid->metaValue<std::string>("container:triangle_provenance"),ids->metaValue<std::string>("container:triangle_provenance"));
  EXPECT_EQ(model.indices,sourceIndices);
  for (size_t i=0;i<model.vertices.size();++i) EXPECT_EQ(model.vertices[i].position,sourceVertices[i].position);
}

TEST_F(OpenVdbMeshConversion, RejectsDegenerateMissingAndNonfiniteTrianglesWithoutOutputs) {
  const std::array<Triangle,1> repeated{{{0,0,1}}},missing{{{0,1,99}}};
  EXPECT_THROW((void)vdb::WriteMeshDistance(square,repeated,path("degenerate.vdb"),processing),std::invalid_argument);
  EXPECT_THROW((void)vdb::WriteMeshDistance(square,missing,path("missing.vdb"),processing),std::invalid_argument);
  auto invalid=square; invalid[0][0]=std::numeric_limits<double>::infinity();
  EXPECT_THROW((void)vdb::WriteMeshDistance(invalid,triangles,path("nonfinite.vdb"),processing),std::invalid_argument);
  auto line=square; line[2]={3,3,0}; line[1]={1,1,0};
  EXPECT_THROW((void)vdb::WriteMeshDistance(line,repeated,path("line.vdb"),processing),std::invalid_argument);
  EXPECT_TRUE(std::filesystem::is_empty(directory));
}

TEST_F(OpenVdbMeshConversion, InvalidNamesOptionsPrecisionAndBudgetsFailBeforePublication) {
  for (double dx:{0.,1e-6,std::numeric_limits<double>::infinity()}) {
    auto p=processing; p.voxelSize=dx; EXPECT_THROW((void)vdb::WriteMeshDistance(square,triangles,path("bad.vdb"),p),std::invalid_argument);
  }
  for (double band:{2.,65.,std::numeric_limits<double>::quiet_NaN()}) {
    auto p=processing; p.halfWidth=band; EXPECT_THROW((void)vdb::WriteMeshDistance(square,triangles,path("bad.vdb"),p),std::invalid_argument);
  }
  auto p=processing; p.maxTriangles=1; EXPECT_THROW((void)vdb::WriteMeshDistance(square,triangles,path("bad.vdb"),p),std::invalid_argument);
  p=processing; p.maxVoxelCount=100; EXPECT_THROW((void)vdb::WriteMeshDistance(square,triangles,path("bad.vdb"),p),std::invalid_argument);
  p=processing; p.maxInputBytes=16*1024*1024; EXPECT_THROW((void)vdb::WriteMeshDistance(square,triangles,path("bad.vdb"),p),std::invalid_argument);
  vdb::MeshConversionOptions c; c.triangleGridName=processing.gridName;
  EXPECT_THROW((void)vdb::WriteMeshDistance(square,triangles,path("bad.vdb"),processing,c),std::invalid_argument);
  auto remote=square; for (auto& point:remote) point[0]+=1e15;
  EXPECT_THROW((void)vdb::WriteMeshDistance(remote,triangles,path("bad.vdb"),processing),std::invalid_argument);
  auto huge=square; huge[0][0]=-1e6; huge[1][0]=1e6; p=processing; p.voxelSize=1.5e-5;
  EXPECT_THROW((void)vdb::WriteMeshDistance(huge,triangles,path("bad.vdb"),p),std::invalid_argument);
  EXPECT_TRUE(std::filesystem::is_empty(directory));
}

TEST_F(OpenVdbMeshConversion, ExistingOutputRemainsByteIdentical) {
  const auto output=path("existing.vdb"); {std::ofstream file(output,std::ios::binary); file<<"preserve";}
  const auto before=bytes(output);
  EXPECT_THROW((void)vdb::WriteMeshDistance(square,triangles,output,processing),std::invalid_argument);
  EXPECT_EQ(bytes(output),before); EXPECT_EQ(std::distance(std::filesystem::directory_iterator(directory),std::filesystem::directory_iterator{}),1);
}

TEST_F(OpenVdbMeshConversion, InvalidModelImportRangesAndTransformsAreNotSilentlyOmitted) {
  namespace dotbim=container::geometry::dotbim; dotbim::Model model; model.vertices.resize(3);
  model.vertices[0].position={0,0,0}; model.vertices[1].position={1,0,0}; model.vertices[2].position={0,1,0};
  model.indices={0,1,2}; model.meshRanges.push_back({.meshId=1,.firstIndex=0,.indexCount=3});
  model.elements.push_back({.meshId=1});
  model.importReport.completeness=container::geometry::ImportCompleteness::Partial;
  EXPECT_THROW((void)vdb::WriteMeshDistance(model,path("partial.vdb"),processing),std::invalid_argument);
  model.importReport={}; model.elements[0].geometryKind=dotbim::GeometryKind::Points;
  EXPECT_THROW((void)vdb::WriteMeshDistance(model,path("points.vdb"),processing),std::invalid_argument);
  model.elements[0].geometryKind=dotbim::GeometryKind::Mesh; model.elements[0].transform[0][3]=.5;
  EXPECT_THROW((void)vdb::WriteMeshDistance(model,path("projective.vdb"),processing),std::invalid_argument);
  model.elements[0].transform=glm::mat4(1); model.meshRanges[0].indexCount=4;
  EXPECT_THROW((void)vdb::WriteMeshDistance(model,path("range.vdb"),processing),std::invalid_argument);
  EXPECT_TRUE(std::filesystem::is_empty(directory));
}
#endif
} // namespace
