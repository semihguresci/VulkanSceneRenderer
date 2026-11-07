#include "Container/geometry/OpenVdbPointMask.h"
#include "Container/geometry/OpenVdbPoints.h"
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <fstream>
#include <limits>

#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
#include <openvdb/openvdb.h>
#include <openvdb/io/File.h>
#include <openvdb/points/PointGroup.h>
#endif

namespace {
namespace vdb=container::geometry::vdb;
TEST(OpenVdbPointMaskAvailability, DisabledBeforeOptionsSelectionsAndPaths) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  GTEST_SKIP();
#else
  vdb::ProcessingOptions p;p.maxInputBytes=0;p.gridName="";
  try{(void)vdb::WritePointVoxelMask("missing","missing/out",{{"missing"}},{true,""},{},p);FAIL();}
  catch(const std::runtime_error& e){EXPECT_NE(std::string(e.what()).find("ENABLE_OPENVDB=ON"),std::string::npos);}
#endif
}

#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
namespace points=openvdb::points;using Grid=points::PointDataGrid;using Op=vdb::PointEditOperation;
class OpenVdbPointMask:public ::testing::Test {
 protected:
  void SetUp() override {
    static std::atomic_uint64_t serial{0};directory=std::filesystem::temp_directory_path()/("container-point-mask-tests-"+
      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+"-"+std::to_string(++serial));
    std::filesystem::create_directory(directory);p.voxelSize=1;p.gridName="points";
    std::vector<vdb::PointRecord> records(5);
    for(size_t n=0;n<5;++n){records[n].position={std::array<double,5>{-16.2,-16.1,.1,.2,32}[n],0,0};records[n].radius=double(n+1);}
    (void)vdb::WritePoints(records,path("raw.vdb"),p);
    const std::array<vdb::PointAttributeEdit,1> attributes{{{Op::Add,"id",{},std::vector<int64_t>{INT64_MIN,9007199254740993ll,INT64_MAX,42,43}}}};
    const std::array<vdb::PointGroupEdit,4> groups{{{Op::Add,"A",{},{1,1,0,1,0}},{Op::Add,"B",{},{0,1,1,1,0}},
      {Op::Add,"C",{},{0,0,0,1,0}},{Op::Add,"empty",{},{0}}}};
    (void)vdb::WritePointAttributeEdits(path("raw.vdb"),path("source.vdb"),attributes,groups,{},p);
    auto source=readPoint("source.vdb");source->tree().setActiveState({-16,0,0},false);
    source->transform().postRotate(.4,openvdb::math::Z_AXIS);source->transform().postTranslate({1e6+.125,2,3});
    source->insertMeta("artist",openvdb::StringMetadata(std::string("raw\0metadata",12)));
    source->insertMeta("precise",openvdb::Vec3DMetadata({1e-200,1e200,-0.0}));
    source->insertMeta("container:point_voxel_note",openvdb::StringMetadata("custom namespace payload"));
    source->insertMeta("container:point_empty_note",openvdb::StringMetadata("ordinary custom metadata"));
    points::makeDescriptorUnique(source->tree())->getMetadata().insertMeta("attribute-only",openvdb::StringMetadata("not grid metadata"));
    write("source.vdb",{source});p.gridName="occupied";
  }
  void TearDown() override {
    std::error_code ignored;for(const auto& entry:std::filesystem::directory_iterator(directory)) std::filesystem::remove(entry.path(),ignored);
    std::filesystem::remove(directory,ignored);
  }
  auto path(const std::string& n) const{return directory/n;}
  static std::string bytes(const std::filesystem::path& p){std::ifstream input(p,std::ios::binary);return {std::istreambuf_iterator<char>(input),{}};}
  template<class T> typename T::Ptr read(const std::string& file,const std::string& name) {
    openvdb::io::File input(path(file).string());input.open(false);auto grid=openvdb::gridPtrCast<T>(input.readGrid(name));input.close();return grid;
  }
  auto readPoint(const std::string& file){return read<Grid>(file,"points");}
  void write(const std::string& file,const openvdb::GridPtrVec& grids){openvdb::io::File output(path(file).string());output.write(grids);}
  vdb::PointMaskResult apply(const std::string& output,const vdb::PointGroupSelection& selection={},bool counts=true) {
    return vdb::WritePointVoxelMask(path("source.vdb"),path(output),selection,{counts,"counts"},{},p);
  }
  void failure(const std::string& file,const vdb::PointGroupSelection& selection={},const vdb::ProcessingOptions* options=nullptr,
               const vdb::PointAttributeOptions& attributes={}) {
    const auto output="failure-"+std::to_string(++serial)+".vdb";
    EXPECT_THROW((void)vdb::WritePointVoxelMask(path(file),path(output),selection,{true,"counts"},{},options?*options:p,attributes),std::exception);
    EXPECT_FALSE(std::filesystem::exists(path(output)));
  }
  std::filesystem::path directory;vdb::ProcessingOptions p;int serial=0;
};

TEST_F(OpenVdbPointMask, NativeOccupancyAndExactCountsIncludeInactiveVoxels) {
  const auto before=bytes(path("source.vdb"));const auto result=apply("mask.vdb");
  EXPECT_EQ(result.inputCount,5u);EXPECT_EQ(result.selectedCount,5u);EXPECT_EQ(result.selectedActive,3u);EXPECT_EQ(result.selectedInactive,2u);
  EXPECT_EQ(result.occupiedVoxelCount,3u);EXPECT_EQ(result.leafCount,3u);EXPECT_FALSE(result.sourceArchivedSchema);
  EXPECT_EQ(result.maskGridName,"occupied");EXPECT_EQ(result.countGridName,"counts");
  const auto mask=read<openvdb::MaskGrid>("mask.vdb","occupied");const auto counts=read<openvdb::Int64Grid>("mask.vdb","counts");
  EXPECT_EQ(mask->activeVoxelCount(),3u);EXPECT_FALSE(mask->background());EXPECT_EQ(counts->background(),0);
  for(const auto& [coordinate,count]:std::array<std::pair<openvdb::Coord,int64_t>,3>{{{{-16,0,0},2},{{0,0,0},2},{{32,0,0},1}}}) {
    EXPECT_TRUE(mask->tree().isValueOn(coordinate));EXPECT_TRUE(mask->tree().getValue(coordinate));
    EXPECT_TRUE(counts->tree().isValueOn(coordinate));EXPECT_EQ(counts->tree().getValue(coordinate),count);
  }
  EXPECT_FALSE(mask->tree().isValueOn({1,0,0}));EXPECT_FALSE(mask->tree().getValue({1,0,0}));
  EXPECT_EQ(bytes(path("source.vdb")),before);
}

TEST_F(OpenVdbPointMask, UnionIntersectionExclusionAndDuplicateGroupSemantics) {
  auto result=apply("union.vdb",{{"A","B"},{},{"C"}});EXPECT_EQ(result.selectedCount,3u);EXPECT_EQ(result.occupiedVoxelCount,2u);
  EXPECT_EQ(read<openvdb::Int64Grid>("union.vdb","counts")->tree().getValue({-16,0,0}),2);
  result=apply("intersection.vdb",{{},{"A","B"},{"C"}});EXPECT_EQ(result.selectedCount,1u);EXPECT_EQ(result.selectedInactive,1u);
  EXPECT_EQ(read<openvdb::Int64Grid>("intersection.vdb","counts")->tree().getValue({-16,0,0}),1);
  result=apply("duplicates.vdb",{{"A","A"}});EXPECT_EQ(result.selectedCount,3u);
  failure("source.vdb",{{"typo"}});failure("source.vdb",{{},{"typo"}});failure("source.vdb",{{},{},{"typo"}});
}

TEST_F(OpenVdbPointMask, ExactTransformCustomMetadataAndDistinctScalarProvenance) {
  const auto source=readPoint("source.vdb");(void)apply("metadata.vdb");
  for(const auto* name:{"occupied","counts"}) {
    openvdb::io::File input(path("metadata.vdb").string());input.open(false);const auto grid=input.readGrid(name);input.close();
    EXPECT_EQ(grid->transform(),source->transform());EXPECT_EQ(grid->getGridClass(),openvdb::GRID_UNKNOWN);
    EXPECT_EQ(grid->metaValue<std::string>("artist"),source->metaValue<std::string>("artist"));
    EXPECT_EQ(grid->metaValue<openvdb::Vec3d>("precise"),source->metaValue<openvdb::Vec3d>("precise"));
    EXPECT_EQ(grid->metaValue<std::string>("container:point_voxel_note"),"custom namespace payload");
    EXPECT_EQ(grid->metaValue<std::string>("container:point_empty_note"),"ordinary custom metadata");
    EXPECT_FALSE((*grid)["container:point_count"]);EXPECT_FALSE((*grid)["attribute-only"]);
    EXPECT_EQ(grid->metaValue<int64_t>("container:point_voxel_input_count"),5);
    EXPECT_EQ(grid->metaValue<int64_t>("container:point_voxel_selected_inactive"),2);
    EXPECT_EQ(grid->metaValue<std::string>("container:point_voxel_source_grid"),"points");
    EXPECT_EQ(grid->metaValue<std::string>("container:point_voxel_selection"),"v1;0:;0:;0:;");
  }
  EXPECT_EQ(read<openvdb::MaskGrid>("metadata.vdb","occupied")->metaValue<std::string>("container:point_voxel_operation"),"selected-native-point-occupancy");
  EXPECT_EQ(read<openvdb::Int64Grid>("metadata.vdb","counts")->metaValue<std::string>("container:point_voxel_operation"),"selected-native-point-count");
}

TEST_F(OpenVdbPointMask, NoPVelocityRadiusDecodingOrWorldRepartitioning) {
  auto source=readPoint("source.vdb");
  for(auto leaf=source->tree().beginLeaf();leaf;++leaf) {
    points::AttributeWriteHandle<openvdb::Vec3f> position(leaf->attributeArray("P"));
    points::AttributeWriteHandle<openvdb::Vec3d> velocity(leaf->attributeArray("v"));
    points::AttributeWriteHandle<double> radius(leaf->attributeArray("pscale"));
    for(openvdb::Index n=0;n<leaf->pointCount();++n) {
      position.set(n,{std::numeric_limits<float>::quiet_NaN(),10000,0});
      velocity.set(n,{std::numeric_limits<double>::infinity(),0,0});radius.set(n,-1);
    }
  }
  source->setTransform(openvdb::math::Transform::createFrustumTransform(openvdb::BBoxd({-1,-1,-1},{1,1,1}),.5,10,1));
  write("source.vdb",{source});const auto before=bytes(path("source.vdb"));const auto result=apply("native.vdb");
  EXPECT_EQ(result.selectedCount,5u);const auto mask=read<openvdb::MaskGrid>("native.vdb","occupied");
  EXPECT_EQ(mask->transform(),source->transform());EXPECT_FALSE(mask->transform().isLinear());
  EXPECT_TRUE(mask->tree().isValueOn({-16,0,0}));EXPECT_EQ(bytes(path("source.vdb")),before);
}

TEST_F(OpenVdbPointMask, EmptySelectionAndNativeEmptyTreeProduceEmptyMaskWithoutCounts) {
  auto result=apply("empty-selection.vdb",{{"empty"}},false);EXPECT_EQ(result.selectedCount,0u);EXPECT_EQ(result.leafCount,0u);EXPECT_TRUE(result.countGridName.empty());
  auto mask=read<openvdb::MaskGrid>("empty-selection.vdb","occupied");EXPECT_EQ(mask->activeVoxelCount(),0u);EXPECT_EQ(mask->tree().leafCount(),0u);
  auto generation=p;generation.gridName="points";(void)vdb::WritePoints({},path("source-empty.vdb"),generation);
  result=vdb::WritePointVoxelMask(path("source-empty.vdb"),path("empty.vdb"),{},{true,"counts"},{},p);
  EXPECT_EQ(result.inputCount,0u);EXPECT_FALSE(result.sourceArchivedSchema);
  EXPECT_EQ(read<openvdb::Int64Grid>("empty.vdb","counts")->tree().leafCount(),0u);
  failure("source-empty.vdb",{{"missing"}});
}

TEST_F(OpenVdbPointMask, ValidatedEmptyArchiveIsConsumedAndAllArchiveKeysRemoved) {
  auto generation=p;generation.gridName="points";
  const auto filtered=vdb::WriteSelectedPoints(path("source.vdb"),path("archived.vdb"),{{"empty"}},vdb::PointSelectionMode::KeepSelected,{},generation);
  ASSERT_TRUE(filtered.archivedSchema);const auto before=bytes(path("archived.vdb"));
  const auto result=vdb::WritePointVoxelMask(path("archived.vdb"),path("from-archive.vdb"),{{"A"}},{true,"counts"},{},p);
  EXPECT_TRUE(result.sourceArchivedSchema);EXPECT_EQ(result.inputCount,0u);EXPECT_EQ(result.selectedCount,0u);
  for(const auto* name:{"occupied","counts"}) {
    openvdb::io::File input(path("from-archive.vdb").string());input.open(false);const auto grid=input.readGrid(name);input.close();
    for(const auto* key:{"container:point_empty_manifest_v1","container:point_empty_descriptor_v1","container:point_empty_leaf_flags_v1"}) EXPECT_FALSE((*grid)[key]);
    EXPECT_EQ(grid->metaValue<std::string>("container:point_empty_note"),"ordinary custom metadata");
    EXPECT_FALSE((*grid)["container:point_count"]);EXPECT_EQ(grid->metaValue<std::string>("artist"),std::string("raw\0metadata",12));
    EXPECT_TRUE(grid->metaValue<bool>("container:point_voxel_archived_source"));
  }
  EXPECT_EQ(bytes(path("archived.vdb")),before);failure("archived.vdb",{{"missing"}});
  auto corrupt=readPoint("archived.vdb");corrupt->insertMeta("container:point_empty_manifest_v1",openvdb::StringMetadata("bad"));write("corrupt.vdb",{corrupt});failure("corrupt.vdb");
  auto conflict=readPoint("source.vdb");conflict->insertMeta("container:point_empty_manifest_v1",openvdb::StringMetadata("bad"));write("conflict.vdb",{conflict});failure("conflict.vdb");
}

TEST_F(OpenVdbPointMask, GroupBackingAndSchemaLimitsAreValidatedBeforeHandles) {
  auto source=readPoint("source.vdb");
  for(auto leaf=source->tree().beginLeaf();leaf;++leaf) {
    const auto index=leaf->attributeSet().groupIndex("A").first;
    auto attributes=std::make_unique<points::AttributeSet>(leaf->attributeSet());
    attributes->replace(index,std::make_shared<points::GroupAttributeArray>(openvdb::Index(leaf->pointCount()),2,true));leaf->replaceAttributeSet(attributes.release());
  }
  write("strided.vdb",{source});failure("strided.vdb",{{"A"}});
  vdb::PointAttributeOptions options;options.maxPoints=4;failure("source.vdb",{},nullptr,options);
  options={};options.maxGroups=1;failure("source.vdb",{},nullptr,options);
  options={};options.maxAttributes=1;failure("source.vdb",{},nullptr,options);
}

TEST_F(OpenVdbPointMask, SharedEncodedLoadedProspectiveMetadataAndTopologyBudgets) {
  auto small=p;small.maxInputBytes=1;failure("source.vdb",{},&small);
  small=p;small.maxVoxelCount=1024;failure("source.vdb",{},&small);
  small=p;small.maxVoxelCount=3*512;
  EXPECT_NO_THROW((void)vdb::WritePointVoxelMask(path("source.vdb"),path("one-grid-cap.vdb"),{},{},{},small));
  failure("source.vdb",{},&small); // Two output grids share the leaf-slot cap.
  small=p;small.maxInputBytes=8*1024*1024;failure("source.vdb",{},&small);
  auto source=readPoint("source.vdb");source->insertMeta("custom-payload",openvdb::StringMetadata(std::string(1024*1024,'x')));write("large-meta.vdb",{source});
  small=p;small.maxInputBytes=96*1024*1024;
  EXPECT_NO_THROW((void)vdb::CountSelectedPoints(path("large-meta.vdb"),{}, {},small));
  failure("large-meta.vdb",{},&small);
  const auto before=bytes(path("source.vdb"));std::ofstream(path("existing.vdb"))<<"preserve";
  EXPECT_THROW((void)vdb::WritePointVoxelMask(path("source.vdb"),path("existing.vdb"),{},{},{},p),std::invalid_argument);
  EXPECT_THROW((void)vdb::WritePointVoxelMask(path("source.vdb"),path("source.vdb"),{},{},{},p),std::invalid_argument);
  EXPECT_EQ(bytes(path("existing.vdb")),"preserve");EXPECT_EQ(bytes(path("source.vdb")),before);
}

TEST_F(OpenVdbPointMask, OutputNamesOptionsAndRegeneratedProvenanceTypes) {
  for(const auto& names:std::array<std::pair<std::string,std::string>,3>{{{"","counts"},{"same","same"},{"valid",std::string(257,'x')}}}) {
    auto options=p;options.gridName=names.first;
    EXPECT_THROW((void)vdb::WritePointVoxelMask(path("source.vdb"),path("bad.vdb"),{},{true,names.second},{},options),std::invalid_argument);
  }
  auto source=readPoint("source.vdb");source->removeMeta("container:point_count");source->insertMeta("container:point_count",openvdb::StringMetadata("old"));
  source->insertMeta("container:point_voxel_input_count",openvdb::StringMetadata("old"));write("source.vdb",{source});
  (void)apply("replaced.vdb");EXPECT_EQ(read<openvdb::MaskGrid>("replaced.vdb","occupied")->metaValue<int64_t>("container:point_voxel_input_count"),5);
  EXPECT_FALSE(read<openvdb::MaskGrid>("replaced.vdb","occupied")->getMetadata<openvdb::StringMetadata>("container:point_count"));
  auto generationIgnored=p;generationIgnored.voxelSize=std::numeric_limits<double>::quiet_NaN();generationIgnored.halfWidth=0;generationIgnored.maxTriangles=0;
  EXPECT_NO_THROW((void)vdb::WritePointVoxelMask(path("source.vdb"),path("unused-options.vdb"),{},{false,""},{},generationIgnored));
}

TEST_F(OpenVdbPointMask, ExplicitDescriptorsSelectDuplicatePointGridNames) {
  const auto source=readPoint("source.vdb");auto other=source->deepCopy();
  for(auto leaf=other->tree().beginLeaf();leaf;++leaf) {
    auto group=leaf->groupWriteHandle("A");for(openvdb::Index n=0;n<leaf->pointCount();++n) group.set(n,false);
  }
  write("duplicates.vdb",{source,other});
  EXPECT_THROW((void)vdb::WritePointVoxelMask(path("duplicates.vdb"),path("ambiguous.vdb"),{{"A"}},{},"points",p),std::invalid_argument);
  const auto result=vdb::WritePointVoxelMask(path("duplicates.vdb"),path("second.vdb"),{{"A"}},{},"descriptor:points[1]",p);
  EXPECT_EQ(result.selectedCount,0u);EXPECT_EQ(result.inputCount,5u);
}
#endif
} // namespace
