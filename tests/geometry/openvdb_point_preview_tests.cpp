#include "Container/geometry/OpenVdbPointPreview.h"
#include "Container/geometry/OpenVdbPoints.h"

#include <gtest/gtest.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <limits>

#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
#include <openvdb/io/File.h>
#include <openvdb/io/Stream.h>
#include <openvdb/points/PointConversion.h>
#endif

namespace {
namespace vdb=container::geometry::vdb;
namespace geometry=container::geometry;
TEST(OpenVdbPointPreviewAvailability, DisabledFailsBeforePathsOrOptions) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  GTEST_SKIP()<<"OpenVDB is enabled";
#else
  try { (void)vdb::LoadPointsPreviewFromFile("missing.vdb",{},-1,{.maxPoints=0}); FAIL(); }
  catch (const std::runtime_error& error) { EXPECT_NE(std::string(error.what()).find("ENABLE_OPENVDB=ON"),std::string::npos); }
#endif
}
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
class OpenVdbPointPreview:public ::testing::Test {
 protected:
  using Grid=openvdb::points::PointDataGrid;
  void SetUp() override {
    openvdb::initialize(); static std::atomic_uint64_t serial{0};
    directory=std::filesystem::temp_directory_path()/("container-point-preview-"+
      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+"-"+std::to_string(++serial));
    std::filesystem::create_directory(directory); processing.gridName="points"; processing.voxelSize=.1;
  }
  void TearDown() override {
    std::error_code ignored;
    for (const auto& entry:std::filesystem::directory_iterator(directory)) std::filesystem::remove(entry.path(),ignored);
    std::filesystem::remove(directory,ignored);
  }
  auto file(const char* name) const { return directory/name; }
  void source(std::vector<vdb::PointRecord> points={{{.2,.3,.4},{1,0,0},.25}},const char* name="points.vdb") {
    (void)vdb::WritePoints(points,file(name),processing);
  }
  Grid::Ptr read(const char* name="points.vdb") {
    openvdb::io::File input(file(name).string()); input.open(false);
    auto result=openvdb::gridPtrCast<Grid>(input.readGrid("points")); input.close(); return result;
  }
  void direct(const char* name,const openvdb::GridPtrVec& grids,bool stats=true) {
    openvdb::io::File output(file(name).string()); output.setGridStatsMetadataEnabled(stats); output.write(grids); output.close();
  }
  static std::string bytes(const std::filesystem::path& path) {
    std::ifstream input(path,std::ios::binary); return {std::istreambuf_iterator<char>(input),{}};
  }
  static std::string property(const geometry::dotbim::Element& element,const std::string& name) {
    for (const auto& field:element.properties) if (field.name==name) return field.value;
    return {};
  }
  std::filesystem::path directory; vdb::ProcessingOptions processing;
};

TEST_F(OpenVdbPointPreview, RadiusMarkersHaveWorldScaleCorrectWindingAndMetadata) {
  source({{{-.4,.2,.3},{1,0,0},.2},{{.6,-.1,.2},{-1,0,0},.5}});
  auto grid=read(); grid->insertMeta("artist",openvdb::StringMetadata("Ada")); direct("input.vdb",{grid});
  const auto original=bytes(file("input.vdb")); const auto records=vdb::ReadPoints(file("input.vdb"),{},processing);
  const auto model=vdb::LoadPointsPreviewFromFile(file("input.vdb"),{},2);
  ASSERT_EQ(model.vertices.size(),12u); ASSERT_EQ(model.indices.size(),48u); ASSERT_EQ(model.elements.size(),1u);
  EXPECT_TRUE(model.nativePointRanges.empty()); EXPECT_EQ(model.elements[0].geometryKind,geometry::dotbim::GeometryKind::Mesh);
  EXPECT_EQ(model.elements[0].type,"OpenVDB Points");
  EXPECT_NE(property(model.elements[0],"Preview").find("point-style controls do not apply"),std::string::npos);
  EXPECT_EQ(model.meshRanges.size(),1u); EXPECT_EQ(model.importReport.importedProductCount,1u);
  EXPECT_EQ(model.importReport.completeness,geometry::ImportCompleteness::Complete);
  EXPECT_TRUE(model.unitMetadata.hasSourceUnits); EXPECT_FALSE(model.unitMetadata.hasMetersPerUnit);
  EXPECT_EQ(model.unitMetadata.sourceUnits,"OpenVDB world units"); EXPECT_FLOAT_EQ(model.unitMetadata.effectiveImportScale,2);
  EXPECT_EQ(property(model.elements[0],"artist"),"Ada"); EXPECT_NE(property(model.elements[0],"Attributes").find("pscale:double"),std::string::npos);
  EXPECT_FALSE(property(model.elements[0],"Source transform").empty());
  for (size_t point=0;point<records.size();++point) {
    const glm::dvec3 center(records[point].position[0]*2,records[point].position[1]*2,records[point].position[2]*2);
    for (size_t vertex=0;vertex<6;++vertex) {
      const auto& value=model.vertices[6*point+vertex];
      EXPECT_NEAR(glm::length(glm::dvec3(value.position)-center),records[point].radius*2,1e-7);
      EXPECT_NEAR(glm::length(value.normal),1,1e-7); EXPECT_NEAR(glm::dot(glm::vec3(value.tangent),value.normal),0,1e-7);
      EXPECT_LE(glm::length(value.position-model.meshRanges[0].boundsCenter),model.meshRanges[0].boundsRadius+1e-6);
    }
    for (size_t face=0;face<8;++face) {
      const glm::dvec3 a(model.vertices[model.indices[24*point+3*face]].position);
      const glm::dvec3 b(model.vertices[model.indices[24*point+3*face+1]].position);
      const glm::dvec3 c(model.vertices[model.indices[24*point+3*face+2]].position);
      EXPECT_GT(glm::dot(glm::cross(b-a,c-a),(a+b+c)/3.-center),0);
    }
  }
  EXPECT_EQ(bytes(file("input.vdb")),original);
}

TEST_F(OpenVdbPointPreview, GridTransformMovesPositionsButRadiusRemainsWorldPayload) {
  source(); auto grid=read();
  auto transform=openvdb::math::Transform::createLinearTransform(.2);
  transform->postScale(openvdb::Vec3d(1,1.5,2)); transform->postRotate(.3,openvdb::math::Z_AXIS);
  transform->postTranslate(openvdb::Vec3d(2,-1,.5)); grid->setTransform(transform);
  direct("transformed.vdb",{grid});
  const auto records=vdb::ReadPoints(file("transformed.vdb"),{},processing);
  const auto model=vdb::LoadPointsPreviewFromFile(file("transformed.vdb"),{},3);
  ASSERT_EQ(model.vertices.size(),6u);
  const glm::dvec3 center(records[0].position[0]*3,records[0].position[1]*3,records[0].position[2]*3);
  for (const auto& value:model.vertices) EXPECT_NEAR(glm::length(glm::dvec3(value.position)-center),records[0].radius*3,5e-7);
}

TEST_F(OpenVdbPointPreview, DirectionColorsDistinguishSignedAndZeroVelocitiesWithoutOverflow) {
  source({{{-.6,0,0},{-1,0,0},.1},{{-.2,0,0},{},.1},{{.2,0,0},{1,0,0},.1},
          {{.6,0,0},{std::numeric_limits<double>::max(),std::numeric_limits<double>::max(),0},.1}});
  const auto model=vdb::LoadPointsPreviewFromFile(file("points.vdb"));
  ASSERT_EQ(model.vertices.size(),24u);
  EXPECT_EQ(model.vertices[0].color,glm::vec3(0,.5,.5)); EXPECT_EQ(model.vertices[6].color,glm::vec3(.5));
  EXPECT_EQ(model.vertices[12].color,glm::vec3(1,.5,.5));
  const auto diagonal=model.vertices[18].color;
  EXPECT_NEAR(diagonal.x,.5+.5/std::sqrt(2.),1e-7); EXPECT_EQ(diagonal.x,diagonal.y); EXPECT_FLOAT_EQ(diagonal.z,.5);
}

TEST_F(OpenVdbPointPreview, MagnitudeRampUsesExplicitSourceUnitsAndUniformModeIsExact) {
  source({{{-.6,0,0},{},.1},{{-.2,0,0},{3,4,0},.1},{{.2,0,0},{10,0,0},.1},
          {{.6,0,0},{std::numeric_limits<double>::max(),std::numeric_limits<double>::max(),0},.1}});
  vdb::PointPreviewOptions options; options.colorMode=vdb::PointPreviewColor::VelocityMagnitude; options.velocityScale=10;
  const auto model=vdb::LoadPointsPreviewFromFile(file("points.vdb"),{},2,options);
  EXPECT_EQ(model.vertices[0].color,glm::vec3(.1,.25,1));
  EXPECT_NEAR(model.vertices[6].color.x,.55,1e-7); EXPECT_NEAR(model.vertices[6].color.y,.225,1e-7); EXPECT_NEAR(model.vertices[6].color.z,.525,1e-7);
  EXPECT_EQ(model.vertices[12].color,glm::vec3(1,.2,.05)); EXPECT_EQ(model.vertices[18].color,model.vertices[12].color);
  EXPECT_NE(property(model.elements[0],"Velocity units").find("independent of importScale"),std::string::npos);
  options.colorMode=vdb::PointPreviewColor::Uniform; options.uniformColor={.2f,.4f,.6f};
  const auto uniform=vdb::LoadPointsPreviewFromFile(file("points.vdb"),{},1,options);
  for (const auto& vertex:uniform.vertices) EXPECT_EQ(vertex.color,glm::vec3(.2,.4,.6));
}

TEST_F(OpenVdbPointPreview, PointTriangleAndStoredTopologyBudgetsAreSharedAcrossGrids) {
  source({{{.1,.2,.3},{},.1},{{.11,.21,.31},{},.1}}); auto a=read(); auto b=read();
  a->setName("a"); b->setName("b"); direct("multi.vdb",{a,b});
  const auto model=vdb::LoadPointsPreviewFromFile(file("multi.vdb"));
  EXPECT_EQ(model.elements.size(),2u); EXPECT_EQ(model.vertices.size(),24u); EXPECT_EQ(model.indices.size()/3,32u);
  vdb::PointPreviewOptions o; o.maxPoints=3;
  EXPECT_THROW((void)vdb::LoadPointsPreviewFromFile(file("multi.vdb"),{},1,o),std::invalid_argument);
  o={}; o.maxTriangles=31;
  EXPECT_THROW((void)vdb::LoadPointsPreviewFromFile(file("multi.vdb"),{},1,o),std::invalid_argument);
  o={}; o.maxActiveVoxels=1;
  EXPECT_THROW((void)vdb::LoadPointsPreviewFromFile(file("multi.vdb"),{},1,o),std::invalid_argument);
  o.maxActiveVoxels=512; // Each grid has one leaf; their combined two leaves exceed one shared leaf.
  EXPECT_THROW((void)vdb::LoadPointsPreviewFromFile(file("multi.vdb"),{},1,o),std::invalid_argument);
  o.maxActiveVoxels=1024; EXPECT_EQ(vdb::LoadPointsPreviewFromFile(file("multi.vdb"),{},1,o).elements.size(),2u);
}

TEST_F(OpenVdbPointPreview, DuplicateSelectorsAndDescriptorCollisionsAreUnambiguous) {
  source(); auto a=read(); auto b=read(); a->setName("field"); b->setName("field"); direct("duplicates.vdb",{a,b});
  EXPECT_THROW((void)vdb::LoadPointsPreviewFromFile(file("duplicates.vdb"),"field"),std::invalid_argument);
  EXPECT_EQ(vdb::LoadPointsPreviewFromFile(file("duplicates.vdb"),"descriptor:field[0]").elements.size(),1u);
  EXPECT_EQ(vdb::LoadPointsPreviewFromFile(file("duplicates.vdb")).elements.size(),2u);
  auto literal=read(); literal->setName("field[0]"); direct("collision.vdb",{a,b,literal});
  EXPECT_THROW((void)vdb::LoadPointsPreviewFromFile(file("collision.vdb")),std::invalid_argument);
  EXPECT_THROW((void)vdb::LoadPointsPreviewFromFile(file("collision.vdb"),"descriptor:field[0]"),std::invalid_argument);
}

TEST_F(OpenVdbPointPreview, MetadataAndRetainedModelMemoryAreBudgeted) {
  source(); auto grid=read(); direct("base.vdb",{grid});
  vdb::PointPreviewOptions small; small.maxInputBytes=20ull*1024*1024; small.maxMetadataBytes=32*1024;
  EXPECT_EQ(vdb::LoadPointsPreviewFromFile(file("base.vdb"),{},1,small).vertices.size(),6u);
  grid->insertMeta("custom",openvdb::StringMetadata(std::string(4096,'x'))); direct("metadata.vdb",{grid});
  EXPECT_THROW((void)vdb::LoadPointsPreviewFromFile(file("metadata.vdb"),{},1,small),std::invalid_argument);
  small.maxMetadataBytes=8*1024*1024;
  grid->insertMeta("custom",openvdb::StringMetadata(std::string(256*1024,'x'))); direct("memory.vdb",{grid});
  EXPECT_THROW((void)vdb::LoadPointsPreviewFromFile(file("memory.vdb"),{},1,small),std::invalid_argument);
  small={}; small.maxInputBytes=1024;
  EXPECT_THROW((void)vdb::LoadPointsPreviewFromFile(file("base.vdb"),{},1,small),std::invalid_argument);
}

TEST_F(OpenVdbPointPreview, DisplayPrecisionRejectsLostRadiusAndUnusableFloatRange) {
  source({{{1e7,0,0},{},.01}}); const auto original=bytes(file("points.vdb"));
  try { (void)vdb::LoadPointsPreviewFromFile(file("points.vdb")); FAIL(); }
  catch (const std::invalid_argument& error) { EXPECT_NE(std::string(error.what()).find("precision"),std::string::npos); }
  EXPECT_EQ(bytes(file("points.vdb")),original);
  source({{{0,0,0},{},1e-40}},"tiny.vdb");
  EXPECT_THROW((void)vdb::LoadPointsPreviewFromFile(file("tiny.vdb")),std::invalid_argument);
  source({{{0,0,0},{},1e300}},"huge.vdb");
  EXPECT_THROW((void)vdb::LoadPointsPreviewFromFile(file("huge.vdb")),std::invalid_argument);
}

TEST_F(OpenVdbPointPreview, StrictAttributesAndSeekableStatisticsRemainRequired) {
  source(); auto grid=read(); openvdb::points::appendAttribute<int32_t>(grid->tree(),"id",1); direct("extra.vdb",{grid});
  EXPECT_THROW((void)vdb::LoadPointsPreviewFromFile(file("extra.vdb")),std::invalid_argument);
  grid=read(); grid->removeMeta(openvdb::GridBase::META_FILE_MEM_BYTES); direct("missing-stats.vdb",{grid},false);
  EXPECT_THROW((void)vdb::LoadPointsPreviewFromFile(file("missing-stats.vdb")),std::invalid_argument);
  std::ofstream output(file("stream.vdb"),std::ios::binary); openvdb::io::Stream stream(output); stream.write({read()}); output.close();
  EXPECT_THROW((void)vdb::LoadPointsPreviewFromFile(file("stream.vdb")),std::invalid_argument);
}

TEST_F(OpenVdbPointPreview, EmptyGridsAreDiagnosedAndInactiveVoxelRecordsAreExplicitlyPreviewed) {
  source({}); auto empty=read(); empty->setName("empty");
  const auto model=vdb::LoadPointsPreviewFromFile(file("points.vdb"));
  EXPECT_TRUE(model.elements.empty()); ASSERT_EQ(model.importReport.diagnostics.size(),1u);
  EXPECT_EQ(model.importReport.skippedProductCount,1u); EXPECT_EQ(model.importReport.completeness,geometry::ImportCompleteness::Partial);
  source({{{.2,.3,.4},{},.1}},"nonempty.vdb"); auto grid=read("nonempty.vdb");
  for (auto leaf=grid->tree().beginLeaf();leaf;++leaf) { const auto point=leaf->beginIndexOn(); if (point) leaf->setValueOff(point.getCoord()); }
  direct("mixed.vdb",{empty,grid});
  const auto mixed=vdb::LoadPointsPreviewFromFile(file("mixed.vdb"));
  EXPECT_EQ(mixed.elements.size(),1u); EXPECT_EQ(mixed.vertices.size(),6u); EXPECT_EQ(mixed.importReport.skippedProductCount,1u);
  EXPECT_NE(property(mixed.elements[0],"Preview").find("including inactive"),std::string::npos);
}

TEST_F(OpenVdbPointPreview, InvalidControlsAndCatalogBudgetsFailExplicitly) {
  source();
  EXPECT_THROW((void)vdb::LoadPointsPreviewFromFile(file("points.vdb"),{},0),std::invalid_argument);
  EXPECT_THROW((void)vdb::LoadPointsPreviewFromFile(file("points.vdb"),{},std::numeric_limits<float>::infinity()),std::invalid_argument);
  vdb::PointPreviewOptions o; o.velocityScale=0;
  EXPECT_THROW((void)vdb::LoadPointsPreviewFromFile(file("points.vdb"),{},1,o),std::invalid_argument);
  o={}; o.uniformColor[0]=std::numeric_limits<float>::quiet_NaN();
  EXPECT_THROW((void)vdb::LoadPointsPreviewFromFile(file("points.vdb"),{},1,o),std::invalid_argument);
  o={}; o.colorMode=static_cast<vdb::PointPreviewColor>(9);
  EXPECT_THROW((void)vdb::LoadPointsPreviewFromFile(file("points.vdb"),{},1,o),std::invalid_argument);
  o={}; o.maxGrids=1; auto scalar=openvdb::FloatGrid::create(); scalar->setName("scalar"); direct("catalog.vdb",{read(),scalar});
  EXPECT_THROW((void)vdb::LoadPointsPreviewFromFile(file("catalog.vdb"),{},1,o),std::invalid_argument);
  EXPECT_THROW((void)vdb::LoadPointsPreviewFromFile(file("catalog.vdb"),"scalar"),std::invalid_argument);
}
#endif
} // namespace
