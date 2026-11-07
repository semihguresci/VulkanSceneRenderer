#include "Container/geometry/OpenVdbPointSelection.h"
#include "Container/geometry/OpenVdbPoints.h"
#include <gtest/gtest.h>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <fstream>
#include <limits>
#include <sstream>
#include <string_view>

#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
#include <openvdb/openvdb.h>
#include <openvdb/io/File.h>
#include <openvdb/points/PointGroup.h>
#include <openvdb/points/PointCount.h>
#endif
namespace {
namespace vdb=container::geometry::vdb;
TEST(OpenVdbPointSelectionAvailability, DisabledBeforePathsOptionsAndSelection) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  GTEST_SKIP();
#else
  for(int mode:{0,1,2})try {
    vdb::ProcessingOptions p;p.maxInputBytes=0;
    if(mode==0)(void)vdb::CountSelectedPoints("missing",{{"missing"}}, {},p);
    if(mode==1)(void)vdb::WriteSelectedPoints("missing","missing/out",{},vdb::PointSelectionMode::KeepSelected,{},p);
    if(mode==2)(void)vdb::MeasurePointAttribute("missing","P",{}, {},p);FAIL();
  } catch(const std::runtime_error& e){EXPECT_NE(std::string(e.what()).find("ENABLE_OPENVDB=ON"),std::string::npos);}
#endif
}
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
namespace points=openvdb::points;using Grid=points::PointDataGrid;using Op=vdb::PointEditOperation;
class OpenVdbPointSelection:public ::testing::Test {
 protected:
  void SetUp()override {
    static std::atomic_uint64_t serial{0};directory=std::filesystem::temp_directory_path()/("container-point-selection-tests-"+
      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+"-"+std::to_string(++serial));std::filesystem::create_directory(directory);
    p.voxelSize=1;p.gridName="points";std::vector<vdb::PointRecord> records(5);
    for(size_t i=0;i<5;++i){records[i].position={std::array<double,5>{-16.2,-16.1,.1,.2,32}[i],0,0};records[i].radius=i+1;records[i].velocity={double(i),2,3};}
    (void)vdb::WritePoints(records,path("raw.vdb"),p);
    const std::array<vdb::PointAttributeEdit,2> attrs{{{Op::Add,"id",{},std::vector<int64_t>{INT64_MIN,9007199254740993ll,INT64_MAX,42,43}},
      {Op::Add,"label",{},std::vector<std::string>{"one","two",std::string("a\0b",3),"four","five"}}}};
    const std::array<vdb::PointGroupEdit,7> groups{{{Op::Add,"A",{},{1,1,0,1,0}},{Op::Add,"B",{},{0,1,1,1,0}},
      {Op::Add,"C",{},{0,0,0,1,0}},{Op::Add,"empty",{},{0}},{Op::Add,"first3",{},{1,1,1,0,0}},
      {Op::Add,"pair",{},{1,1,0,0,0}},{Op::Add,"last",{},{0,0,0,0,1}}}};
    (void)vdb::WritePointAttributeEdits(path("raw.vdb"),path("source.vdb"),attrs,groups,{},p);
    auto grid=read("source.vdb");grid->tree().setActiveState({-16,0,0},false);
    grid->insertMeta("artist",openvdb::StringMetadata(std::string("raw\0metadata",12)));
    grid->insertMeta("precise",openvdb::DoubleMetadata(.1234567891234567));
    auto descriptor=points::makeDescriptorUnique(grid->tree());descriptor->getMetadata().insertMeta("custom:vec",openvdb::Vec3DMetadata({1e-200,1e200,-0.0}));
    for(auto leaf=grid->tree().beginLeaf();leaf;++leaf)leaf->attributeArray("v").setHidden(true);
    write("source.vdb",grid);p.gridName="selected";
  }
  void TearDown()override{std::error_code ignored;for(const auto& entry:std::filesystem::directory_iterator(directory))std::filesystem::remove(entry.path(),ignored);std::filesystem::remove(directory,ignored);}
  auto path(const std::string& n)const{return directory/n;}
  static std::string bytes(const std::filesystem::path& path){std::ifstream stream(path,std::ios::binary);return {std::istreambuf_iterator<char>(stream),{}};}
  Grid::Ptr read(const std::string& n,const std::string& name="points") {openvdb::io::File file(path(n).string());file.open(false);auto grid=openvdb::gridPtrCast<Grid>(file.readGrid(name));file.close();return grid;}
  void write(const std::string& n,const Grid::Ptr& grid){openvdb::io::File file(path(n).string());file.write({grid});}
  template<class T>std::vector<T> column(const std::string& file,const std::string& attribute){return std::get<std::vector<T>>(vdb::ReadPointAttribute(path(file),attribute).values);}
  static std::string descriptorBytes(const Grid& grid){std::ostringstream stream(std::ios::binary);grid.tree().cbeginLeaf()->attributeSet().descriptor().write(stream);return stream.str();}
  void failure(const std::string& input,const vdb::PointGroupSelection& selection={},const vdb::ProcessingOptions* budgets=nullptr){const auto output="failed-"+std::to_string(serial++)+".vdb";
  EXPECT_THROW((void)vdb::WriteSelectedPoints(path(input),path(output),selection,vdb::PointSelectionMode::KeepSelected,{},budgets?*budgets:p),std::exception);EXPECT_FALSE(std::filesystem::exists(path(output)));}
  std::filesystem::path directory;vdb::ProcessingOptions p;int serial=0;
};
TEST_F(OpenVdbPointSelection, UnionIntersectionExclusionCountsIncludeInactivePoints) {
  auto result=vdb::CountSelectedPoints(path("source.vdb"));EXPECT_EQ(result.total,5u);EXPECT_EQ(result.selected,5u);EXPECT_EQ(result.selectedActive,3u);EXPECT_EQ(result.selectedInactive,2u);
  result=vdb::CountSelectedPoints(path("source.vdb"),{{"A","B"},{},{"C"}});EXPECT_EQ(result.selected,3u);EXPECT_EQ(result.selectedActive,1u);EXPECT_EQ(result.selectedInactive,2u);
  result=vdb::CountSelectedPoints(path("source.vdb"),{{},{"A","B"},{"C"}});EXPECT_EQ(result.selected,1u);EXPECT_EQ(result.selectedInactive,1u);
  result=vdb::CountSelectedPoints(path("source.vdb"),{{"A","A"}});EXPECT_EQ(result.selected,3u);
  EXPECT_EQ(vdb::CountSelectedPoints(path("source.vdb"),{{},{},{"A","B"}}).selected,1u);
  EXPECT_THROW((void)vdb::CountSelectedPoints(path("source.vdb"),{{"typo"}}),std::invalid_argument);
  EXPECT_THROW((void)vdb::CountSelectedPoints(path("source.vdb"),{{},{"typo"}}),std::invalid_argument);
}
TEST_F(OpenVdbPointSelection, StridedGroupBackingRejectsBeforeCountingOrCompactingMembership) {
  auto source=read("source.vdb");
  for(auto leaf=source->tree().beginLeaf();leaf;++leaf){const auto index=leaf->attributeSet().groupIndex("A").first;auto attrs=std::make_unique<points::AttributeSet>(leaf->attributeSet());attrs->replace(index,std::make_shared<points::GroupAttributeArray>(openvdb::Index(leaf->pointCount()),2,true));leaf->replaceAttributeSet(attrs.release());}
  write("strided-groups.vdb",source);const auto before=bytes(path("strided-groups.vdb"));
  try{(void)vdb::CountSelectedPoints(path("strided-groups.vdb"),{{"A"}});FAIL();}catch(const std::invalid_argument& e){EXPECT_NE(std::string(e.what()).find("group backing arrays must have constant stride one"),std::string::npos);}
  failure("strided-groups.vdb",{{"A"}});EXPECT_EQ(bytes(path("strided-groups.vdb")),before);
}
TEST_F(OpenVdbPointSelection, KeepDeletePreserveExactIdsNativeOrderInactiveLeavesAndGroups) {
  const auto before=bytes(path("source.vdb"));auto source=read("source.vdb");
  const auto result=vdb::WriteSelectedPoints(path("source.vdb"),path("keep.vdb"),{{"A","B"},{},{"C"}},vdb::PointSelectionMode::KeepSelected,{},p);
  EXPECT_EQ(result.inputCount,5u);EXPECT_EQ(result.selectedCount,3u);EXPECT_EQ(result.outputCount,3u);EXPECT_FALSE(result.archivedSchema);
  EXPECT_EQ(column<int64_t>("keep.vdb","id"),(std::vector<int64_t>{INT64_MIN,9007199254740993ll,INT64_MAX}));
  EXPECT_EQ(column<std::string>("keep.vdb","label"),(std::vector<std::string>{"one","two",std::string("a\0b",3)}));
  EXPECT_EQ(vdb::ReadPointGroup(path("keep.vdb"),"A"),(std::vector<uint8_t>{1,1,0}));
  auto actual=read("keep.vdb","selected");ASSERT_TRUE(actual->tree().probeConstLeaf({-16,0,0}));
  EXPECT_EQ(actual->metaValue<std::string>("artist"),source->metaValue<std::string>("artist"));EXPECT_EQ(actual->metaValue<double>("precise"),source->metaValue<double>("precise"));
  EXPECT_EQ(actual->transform(),source->transform());EXPECT_EQ(descriptorBytes(*actual),descriptorBytes(*source));
  EXPECT_FALSE(actual->tree().isValueOn({-16,0,0}));EXPECT_EQ(vdb::CountSelectedPoints(path("keep.vdb")).selectedInactive,2u);
  for(auto leaf=actual->tree().cbeginLeaf();leaf;++leaf){leaf->validateOffsets();EXPECT_TRUE(leaf->constAttributeArray("v").isHidden());}
  const auto deleted=vdb::WriteSelectedPoints(path("source.vdb"),path("delete.vdb"),{{},{"A","B"},{"C"}},vdb::PointSelectionMode::DeleteSelected,{},p);
  EXPECT_EQ(deleted.outputCount,4u);EXPECT_EQ(column<int64_t>("delete.vdb","id"),(std::vector<int64_t>{INT64_MIN,INT64_MAX,42,43}));
  EXPECT_EQ(bytes(path("source.vdb")),before);
}
TEST_F(OpenVdbPointSelection, RegisteredPackedAndStridedStorageArePreservedWithoutCodecConversion) {
  using Packed=points::TypedAttributeArray<float,points::TruncateCodec>;if(!Packed::isRegistered())Packed::registerType();
  auto source=read("source.vdb");points::appendAttribute<float,points::TruncateCodec>(source->tree(),"packed",.75f);
  points::appendAttribute<int64_t>(source->tree(),"pair_id",0,2);
  size_t ordinal=0;
  for(auto leaf=source->tree().beginLeaf();leaf;++leaf){points::AttributeWriteHandle<int64_t> handle(leaf->attributeArray("pair_id"));points::AttributeWriteHandle<float,points::TruncateCodec> packed(leaf->attributeArray("packed"));for(openvdb::Index n=0;n<leaf->pointCount();++n,++ordinal){handle.set(n,0,INT64_MAX-int64_t(ordinal));handle.set(n,1,INT64_MIN+int64_t(ordinal));packed.set(n,float(ordinal)+.12345f);}leaf->attributeArray("packed").setHidden(true);}
  source->transform().postRotate(.4,openvdb::math::Z_AXIS);source->transform().postTranslate({1e6+.125,2,3});write("packed.vdb",source);
  (void)vdb::WriteSelectedPoints(path("packed.vdb"),path("packed-kept.vdb"),{{"A"}},vdb::PointSelectionMode::KeepSelected,{},p);
  const auto actual=read("packed-kept.vdb","selected");EXPECT_EQ(actual->transform(),source->transform());EXPECT_EQ(descriptorBytes(*actual),descriptorBytes(*source));
  const std::array<int64_t,3> expectedOrdinals{0,1,3};ordinal=0;
  for(auto leaf=actual->tree().cbeginLeaf();leaf;++leaf){EXPECT_TRUE(leaf->constAttributeArray("packed").isType<Packed>());EXPECT_TRUE(leaf->constAttributeArray("packed").isHidden());points::AttributeHandle<int64_t> pair(leaf->constAttributeArray("pair_id"));points::AttributeHandle<float,points::TruncateCodec> packed(leaf->constAttributeArray("packed"));for(openvdb::Index n=0;n<leaf->pointCount();++n,++ordinal){const auto original=expectedOrdinals[ordinal];EXPECT_EQ(pair.get(n,0),INT64_MAX-original);EXPECT_EQ(pair.get(n,1),INT64_MIN+original);const auto* originalLeaf=source->tree().probeConstLeaf(leaf->origin());points::AttributeHandle<float,points::TruncateCodec> originalPacked(originalLeaf->constAttributeArray("packed"));EXPECT_EQ(packed.get(n),originalPacked.get(original==3?1:n));}}
}
TEST_F(OpenVdbPointSelection, PartialFilteringPreservesSignedZerosAndUniformStrideTwoStorage) {
  auto source=read("source.vdb");points::appendAttribute<double>(source->tree(),"signed_zero",0.0);
  points::appendAttribute<double>(source->tree(),"uniform_pair",-.75,2);
  auto leaf=source->tree().probeLeaf({-16,0,0});ASSERT_NE(leaf,nullptr);
  points::AttributeWriteHandle<double> zero(leaf->attributeArray("signed_zero"));zero.set(0,+0.0);zero.set(1,-0.0);
  // Three points in one leaf are needed so a partial filter retains both signs.
  // Reuse the two-point inactive leaf plus delete its other leaf peers: this
  // forces copying in the middle leaf separately; a dedicated compact fixture
  // below exercises both signs in the same partially filtered leaf.
  write("uniform-stride.vdb",source);
  (void)vdb::WriteSelectedPoints(path("uniform-stride.vdb"),path("uniform-kept.vdb"),{{"B"}},vdb::PointSelectionMode::KeepSelected,{},p);
  const auto uniform=read("uniform-kept.vdb","selected");
  for(auto l=uniform->tree().cbeginLeaf();l;++l){points::AttributeHandle<double> pair(l->constAttributeArray("uniform_pair"));for(openvdb::Index n=0;n<l->pointCount();++n){EXPECT_DOUBLE_EQ(pair.get(n,0),-.75);EXPECT_DOUBLE_EQ(pair.get(n,1),-.75);}}
  std::vector<vdb::PointRecord> records(3);for(auto& point:records)point.radius=1;
  auto generation=p;generation.gridName="points";(void)vdb::WritePoints(records,path("zeros.vdb"),generation);
  const std::array<vdb::PointAttributeEdit,1> edits{{{Op::Add,"z",{},std::vector<double>{+0.0,-0.0,5.0}}}};
  const std::array<vdb::PointGroupEdit,1> groups{{{Op::Add,"keep",{},{1,1,0}}}};
  (void)vdb::WritePointAttributeEdits(path("zeros.vdb"),path("zeros-ready.vdb"),edits,groups,{},generation);
  // P03 assignment's own compaction can collapse signed zeros only if all its
  // values compare equal; the third5.0 keeps this source explicitly expanded.
  (void)vdb::WriteSelectedPoints(path("zeros-ready.vdb"),path("zeros-kept.vdb"),{{"keep"}},vdb::PointSelectionMode::KeepSelected,{},p);
  const auto values=column<double>("zeros-kept.vdb","z");ASSERT_EQ(values.size(),2u);
  EXPECT_EQ(std::bit_cast<uint64_t>(values[0]),std::bit_cast<uint64_t>(+0.0));EXPECT_EQ(std::bit_cast<uint64_t>(values[1]),std::bit_cast<uint64_t>(-0.0));
}
TEST_F(OpenVdbPointSelection, EmptySelectionProducesValidZeroTreeAndLosslessOpaqueSchemaArchive) {
  const auto source=read("source.vdb");source->tree().beginLeaf()->attributeArray("v").setHidden(false);write("source.vdb",source);
  const auto exact=descriptorBytes(*source);
  const auto result=vdb::WriteSelectedPoints(path("source.vdb"),path("empty.vdb"),{{"empty"}},vdb::PointSelectionMode::KeepSelected,{},p);
  EXPECT_EQ(result.outputCount,0u);EXPECT_TRUE(result.archivedSchema);EXPECT_EQ(result.schema.pointCount,0u);EXPECT_EQ(result.schema.attributes.size(),source->tree().cbeginLeaf()->attributeSet().size());
  const auto actual=read("empty.vdb","selected");EXPECT_EQ(actual->tree().leafCount(),0u);EXPECT_EQ(points::pointCount(actual->tree()),0u);EXPECT_EQ(actual->activeVoxelCount(),0u);
  const auto archive=actual->metaValue<std::string>("container:point_empty_descriptor_v1");EXPECT_EQ(archive,exact);
  // Independent native read validates all registered descriptor metadata/types,
  // including embedded NULs and native StringCodec dictionary entries.
  std::istringstream stream(archive,std::ios::binary);points::AttributeSet::Descriptor decoded;decoded.read(stream);
  EXPECT_EQ(decoded,source->tree().cbeginLeaf()->attributeSet().descriptor());
  const auto flags=actual->metaValue<std::string>("container:point_empty_leaf_flags_v1");
  constexpr std::string_view magic="Container.PointEmpty.LeafFlags.1";
  EXPECT_EQ(flags.substr(0,magic.size()),magic);
  size_t flagOffset=magic.size()+16;
  const auto integer=[&](){uint32_t value=0;for(size_t b=0;b<4;++b)value|=uint32_t(uint8_t(flags.at(flagOffset++)))<<(8*b);return value;};
  for(auto leaf=source->tree().cbeginLeaf();leaf;++leaf){for(size_t axis=0;axis<3;++axis)EXPECT_EQ(int32_t(integer()),leaf->origin()[axis]);for(const auto& a:result.schema.attributes){const auto& native=leaf->constAttributeArray(a.name);EXPECT_EQ(uint8_t(flags.at(flagOffset++)),native.flags()&uint8_t(points::AttributeArray::HIDDEN|points::AttributeArray::CONSTANTSTRIDE));EXPECT_EQ(integer(),native.stride());}}
  EXPECT_EQ(flagOffset,flags.size());
  EXPECT_EQ(actual->metaValue<std::string>("artist"),source->metaValue<std::string>("artist"));
  const auto count=vdb::CountSelectedPoints(path("empty.vdb"),{{"A"}});EXPECT_TRUE(count.archivedSchema);EXPECT_EQ(count.total,0u);EXPECT_EQ(count.selected,0u);EXPECT_EQ(count.schema.groups,result.schema.groups);
  EXPECT_EQ(count.schema.attributes.size(),result.schema.attributes.size());EXPECT_EQ(count.schema.gridName,"selected");
  const auto stats=vdb::MeasurePointAttribute(path("empty.vdb"),"id");EXPECT_FALSE(stats.hasValues);EXPECT_EQ(stats.selectedCount,0u);EXPECT_TRUE(std::get<std::vector<int64_t>>(stats.minimum).empty());
  const auto copied=vdb::WriteSelectedPoints(path("empty.vdb"),path("empty-copy.vdb"),{},vdb::PointSelectionMode::DeleteSelected,{},p);EXPECT_TRUE(copied.archivedSchema);
  EXPECT_EQ(read("empty-copy.vdb","selected")->metaValue<std::string>("container:point_empty_descriptor_v1"),exact);
  const auto all=vdb::WriteSelectedPoints(path("source.vdb"),path("deleted-all.vdb"),{},vdb::PointSelectionMode::DeleteSelected,{},p);EXPECT_TRUE(all.archivedSchema);EXPECT_EQ(all.outputCount,0u);
}
TEST_F(OpenVdbPointSelection, EmptyArchiveValidationRejectsForeignCorruptionConflictsAndBudgets) {
  (void)vdb::WriteSelectedPoints(path("source.vdb"),path("empty.vdb"),{},vdb::PointSelectionMode::DeleteSelected,{},p);
  auto empty=read("empty.vdb","selected");auto descriptor=empty->metaValue<std::string>("container:point_empty_descriptor_v1");descriptor[0]^=1;
  empty->insertMeta("container:point_empty_descriptor_v1",openvdb::StringMetadata(descriptor));write("corrupt.vdb",empty);
  EXPECT_THROW((void)vdb::CountSelectedPoints(path("corrupt.vdb")),std::invalid_argument);
  empty=read("empty.vdb","selected");auto manifest=empty->metaValue<std::string>("container:point_empty_manifest_v1");manifest.resize(8);
  empty->insertMeta("container:point_empty_manifest_v1",openvdb::StringMetadata(manifest));write("truncated.vdb",empty);
  EXPECT_THROW((void)vdb::CountSelectedPoints(path("truncated.vdb")),std::invalid_argument);
  auto source=read("source.vdb");source->insertMeta("container:point_empty_manifest_v1",openvdb::Int64Metadata(4));write("conflict.vdb",source);failure("conflict.vdb");
  vdb::PointAttributeOptions limits;limits.maxStringBytes=1;
  EXPECT_THROW((void)vdb::CountSelectedPoints(path("empty.vdb"),{}, {},p,limits),std::invalid_argument);
  limits={};limits.maxAttributes=1;
  EXPECT_THROW((void)vdb::CountSelectedPoints(path("empty.vdb"),{}, {},p,limits),std::invalid_argument);
}
TEST_F(OpenVdbPointSelection, NumericStatisticsHaveExactExtremaAndPopulationUnitsAcrossAllSevenTypes) {
  const std::array<vdb::PointAttributeEdit,7> edits{{{Op::Add,"flag",{},std::vector<uint8_t>{0,1,1,0,0}},
    {Op::Add,"i32",{},std::vector<int32_t>{-3,0,3,1,1}},{Op::Add,"near",{},std::vector<int64_t>{INT64_MAX-2,INT64_MAX-1,INT64_MAX,0,0}},
    {Op::Add,"f32",{},std::vector<float>{-3,0,3,1,1}},{Op::Add,"f64",{},std::vector<double>{-3,0,3,1,1}},
    {Op::Add,"v32",{},std::vector<std::array<float,3>>{{-3,2,7},{0,2,7},{3,2,7},{1,2,7},{1,2,7}}},
    {Op::Add,"v64",{},std::vector<std::array<double,3>>{{-3,2,7},{0,2,7},{3,2,7},{1,2,7},{1,2,7}}}}};
  (void)vdb::WritePointAttributeEdits(path("source.vdb"),path("statistics.vdb"),edits,{}, {},p);
  const vdb::PointGroupSelection first{{"first3"}};
  for(const auto* name:{"i32","f32","f64","v32","v64"}){const auto s=vdb::MeasurePointAttribute(path("statistics.vdb"),name,first);EXPECT_TRUE(s.hasValues);EXPECT_EQ(s.selectedCount,3u);EXPECT_DOUBLE_EQ(s.mean[0],0);EXPECT_NEAR(s.populationStdDev[0],std::sqrt(6.),1e-14);if(s.componentCount==3){EXPECT_DOUBLE_EQ(s.mean[1],2);EXPECT_DOUBLE_EQ(s.populationStdDev[1],0);}}
  const auto flag=vdb::MeasurePointAttribute(path("statistics.vdb"),"flag",first);EXPECT_NEAR(flag.mean[0],2./3,1e-15);EXPECT_NEAR(flag.populationStdDev[0],std::sqrt(2.)/3,1e-15);
  const auto nearStats=vdb::MeasurePointAttribute(path("statistics.vdb"),"near",first);EXPECT_EQ(std::get<std::vector<int64_t>>(nearStats.minimum)[0],INT64_MAX-2);EXPECT_EQ(std::get<std::vector<int64_t>>(nearStats.maximum)[0],INT64_MAX);EXPECT_NEAR(nearStats.populationStdDev[0],std::sqrt(2./3),1e-15);
}
TEST_F(OpenVdbPointSelection, ExtremeCancellationTinyVarianceAndOppositeInt64MeansRemainRepresentable) {
  const auto maximum=std::numeric_limits<double>::max();
  const std::array<vdb::PointAttributeEdit,4> edits{{{Op::Add,"extreme",{},std::vector<double>{-maximum,1e-200,maximum,0,0}},
    {Op::Add,"tiny",{},std::vector<double>{0,2e-300,4e-300,0,0}},{Op::Add,"opposite",{},std::vector<int64_t>{INT64_MIN,INT64_MAX,0,0,0}},
    {Op::Add,"vector",{},std::vector<std::array<double,3>>{{-maximum,1,-2},{1e-200,3,0},{maximum,5,2},{0,0,0},{0,0,0}}}}};
  (void)vdb::WritePointAttributeEdits(path("source.vdb"),path("extreme.vdb"),edits,{}, {},p);
  const auto extreme=vdb::MeasurePointAttribute(path("extreme.vdb"),"extreme",{{"first3"}});EXPECT_NEAR(extreme.mean[0]/1e-200,1./3,1e-15);EXPECT_NEAR(extreme.populationStdDev[0]/maximum,std::sqrt(2./3),1e-15);
  const auto tiny=vdb::MeasurePointAttribute(path("extreme.vdb"),"tiny",{{"first3"}});EXPECT_DOUBLE_EQ(tiny.mean[0],2e-300);EXPECT_NEAR(tiny.populationStdDev[0]/1e-300,std::sqrt(8./3),1e-15);
  const auto opposite=vdb::MeasurePointAttribute(path("extreme.vdb"),"opposite",{{"pair"}});EXPECT_DOUBLE_EQ(opposite.mean[0],-.5);EXPECT_DOUBLE_EQ(opposite.populationStdDev[0],0x1p63);
  const auto vector=vdb::MeasurePointAttribute(path("extreme.vdb"),"vector",{{"first3"}});EXPECT_NEAR(vector.mean[0]/1e-200,1./3,1e-15);EXPECT_DOUBLE_EQ(vector.mean[1],3);EXPECT_DOUBLE_EQ(vector.mean[2],0);EXPECT_NEAR(vector.populationStdDev[1],std::sqrt(8./3),1e-15);
}
TEST_F(OpenVdbPointSelection, EmptyStatisticsUnsupportedTypesSelectedNonfiniteAndFinalUnderflowAreExplicit) {
  const auto empty=vdb::MeasurePointAttribute(path("source.vdb"),"id",{{"empty"}});EXPECT_FALSE(empty.hasValues);EXPECT_EQ(empty.selectedCount,0u);EXPECT_TRUE(std::get<std::vector<int64_t>>(empty.minimum).empty());
  EXPECT_THROW((void)vdb::MeasurePointAttribute(path("source.vdb"),"P"),std::invalid_argument);
  EXPECT_THROW((void)vdb::MeasurePointAttribute(path("source.vdb"),"label"),std::invalid_argument);
  auto source=read("source.vdb");points::appendAttribute<double>(source->tree(),"bad",0);
  for(auto leaf=source->tree().beginLeaf();leaf;++leaf)if(leaf->origin().x()==32){points::AttributeWriteHandle<double> value(leaf->attributeArray("bad"));value.set(0,std::numeric_limits<double>::quiet_NaN());}
  write("nonfinite.vdb",source);
  EXPECT_NO_THROW((void)vdb::MeasurePointAttribute(path("nonfinite.vdb"),"bad",{{"first3"}}));
  EXPECT_THROW((void)vdb::MeasurePointAttribute(path("nonfinite.vdb"),"bad"),std::invalid_argument);
  const std::array<vdb::PointAttributeEdit,1> edits{{{Op::Add,"denorm",{},std::vector<double>{std::numeric_limits<double>::denorm_min(),0,0,0,0}}}};
  (void)vdb::WritePointAttributeEdits(path("source.vdb"),path("denorm.vdb"),edits,{}, {},p);
  EXPECT_THROW((void)vdb::MeasurePointAttribute(path("denorm.vdb"),"denorm",{{"first3"}}),std::runtime_error);
}
TEST_F(OpenVdbPointSelection, NoOpCopiesDynamicStrideButDeletionRejectsAndBudgetsProtectOutput) {
  auto source=read("source.vdb");points::appendAttribute<int32_t>(source->tree(),"dynamic",0,2,false);write("dynamic.vdb",source);
  const auto count=vdb::WriteSelectedPoints(path("dynamic.vdb"),path("no-delete.vdb"),{{"empty"}},vdb::PointSelectionMode::DeleteSelected,{},p);EXPECT_EQ(count.outputCount,5u);
  failure("dynamic.vdb",{{"A"}});failure("dynamic.vdb",{{"empty"}});
  auto tiny=p;tiny.maxInputBytes=1;failure("source.vdb",{},&tiny);tiny=p;tiny.maxVoxelCount=512;failure("source.vdb",{},&tiny);
  const auto before=bytes(path("source.vdb"));
  EXPECT_THROW((void)vdb::WriteSelectedPoints(path("source.vdb"),path("source.vdb")),std::invalid_argument);EXPECT_EQ(bytes(path("source.vdb")),before);
  EXPECT_THROW((void)vdb::WriteSelectedPoints(path("source.vdb"),path("bad-mode.vdb"),{},static_cast<vdb::PointSelectionMode>(123)),std::invalid_argument);EXPECT_FALSE(std::filesystem::exists(path("bad-mode.vdb")));
}
TEST_F(OpenVdbPointSelection, UniformHugeStrideExpansionIsPreflightedBeforeNativeAllocation) {
  auto source=read("source.vdb");points::appendAttribute<float>(source->tree(),"wide",1.0f,100'000'000);
  write("wide.vdb",source);EXPECT_LT(std::filesystem::file_size(path("wide.vdb")),1024u*1024u);
  try{(void)vdb::WriteSelectedPoints(path("wide.vdb"),path("wide-selected.vdb"),{{},{"A","B"},{"C"}},vdb::PointSelectionMode::KeepSelected,{},p);FAIL();}
  catch(const std::invalid_argument& e){EXPECT_NE(std::string(e.what()).find("expanded strided attribute"),std::string::npos);}
  EXPECT_FALSE(std::filesystem::exists(path("wide-selected.vdb")));
}
TEST_F(OpenVdbPointSelection, OriginallyEmptyTreesCopyWithoutInventingArchivedAttributes) {
  auto empty=p;empty.gridName="points";(void)vdb::WritePoints({},path("original-empty.vdb"),empty);
  const auto count=vdb::CountSelectedPoints(path("original-empty.vdb"));EXPECT_EQ(count.total,0u);EXPECT_EQ(count.selected,0u);EXPECT_FALSE(count.archivedSchema);
  const auto copied=vdb::WriteSelectedPoints(path("original-empty.vdb"),path("original-empty-copy.vdb"),{},vdb::PointSelectionMode::KeepSelected,{},p);
  EXPECT_EQ(copied.outputCount,0u);EXPECT_FALSE(copied.archivedSchema);EXPECT_TRUE(copied.schema.attributes.empty());
}
#endif
} // namespace

