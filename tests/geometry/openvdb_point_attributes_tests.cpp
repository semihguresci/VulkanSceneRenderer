#include "Container/geometry/OpenVdbPointAttributes.h"
#include "Container/geometry/OpenVdbPoints.h"
#include <gtest/gtest.h>
#include <algorithm>
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
#include <openvdb/points/PointDataGrid.h>
#include <openvdb/points/PointAttribute.h>
#include <openvdb/points/PointGroup.h>
#endif

namespace {
namespace vdb=container::geometry::vdb;
using Op=vdb::PointEditOperation;
TEST(OpenVdbPointAttributesAvailability, DisabledBeforePathsAndOptions) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  GTEST_SKIP();
#else
  for (int mode:{0,1,2,3}) {
    try {
      if (mode==0) (void)vdb::InspectPointAttributes("missing.vdb",{}, {.maxVoxelCount=0});
      if (mode==1) (void)vdb::ReadPointAttribute("missing.vdb","",{}, {.maxVoxelCount=0});
      if (mode==2) (void)vdb::ReadPointGroup("missing.vdb","",{}, {.maxVoxelCount=0});
      if (mode==3) (void)vdb::WritePointAttributeEdits("missing.vdb","missing/output.vdb",{}, {},{}, {.maxVoxelCount=0});
      FAIL();
    } catch (const std::runtime_error& error) {EXPECT_NE(std::string(error.what()).find("ENABLE_OPENVDB=ON"),std::string::npos);}
  }
#endif
}
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
namespace points=openvdb::points;
using Grid=points::PointDataGrid;
class ExactPointFixture final: public Grid {
public:
  explicit ExactPointFixture(const Grid& source):Grid(openvdb::ConstPtrCast<Grid::TreeType>(source.constTreePtr())) {
    insertMeta(source); setTransform(std::make_shared<openvdb::math::Transform>(source.transform()));
  }
  openvdb::GridBase::ConstPtr copyGrid() const override {
    return copyReplacingMetadataAndTransform(*this,std::make_shared<openvdb::math::Transform>(transform()));
  }
  openvdb::GridBase::Ptr copyGrid() override {
    return openvdb::ConstPtrCast<openvdb::GridBase>(static_cast<const ExactPointFixture&>(*this).copyGrid());
  }
};
class OpenVdbPointAttributes:public ::testing::Test {
 protected:
  void SetUp() override {
    openvdb::initialize(); static std::atomic_uint64_t serial{0};
    directory=std::filesystem::temp_directory_path()/("container-point-attributes-test-"+
      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+"-"+std::to_string(++serial));
    std::filesystem::create_directory(directory); processing.gridName="edited";
    const std::array<vdb::PointRecord,4> input{{{{32,0,0},{1,2,3},32},{{-16,0,0},{4,5,6},16},
      {{.1,0,0},{7,8,9},1},{{.2,0,0},{10,11,12},2}}};
    auto p=processing; p.gridName="points"; p.voxelSize=1; (void)vdb::WritePoints(input,path("input.vdb"),p);
    auto grid=read("input.vdb"); grid->tree().setActiveState({0,0,0},false);
    points::appendGroup(grid->tree(),"keep"); points::setGroup(grid->tree(),"keep",true);
    auto descriptor=points::makeDescriptorUnique(grid->tree());
    descriptor->getMetadata().insertMeta("custom:descriptor",openvdb::StringMetadata("preserve dictionary metadata"));
    grid->insertMeta("custom:grid",openvdb::StringMetadata("preserve grid metadata"));
    for (auto leaf=grid->tree().beginLeaf();leaf;++leaf) leaf->attributeArray("v").setHidden(true);
    write("input.vdb",grid);
  }
  void TearDown() override {
    std::error_code ignored; for (const auto& entry:std::filesystem::directory_iterator(directory)) std::filesystem::remove(entry.path(),ignored);
    std::filesystem::remove(directory,ignored);
  }
  auto path(const std::string& name) const {return directory/name;}
  Grid::Ptr read(const std::string& name,const std::string& gridName="points") {
    openvdb::io::File file(path(name).string()); file.open(false); auto grid=openvdb::gridPtrCast<Grid>(file.readGrid(gridName)); file.close(); return grid;
  }
  void write(const std::string& name,const Grid::Ptr& grid) {openvdb::io::File file(path(name).string()); file.write({grid});}
  static std::string bytes(const std::filesystem::path& file) {std::ifstream input(file,std::ios::binary); return {std::istreambuf_iterator<char>(input),{}};}
  template<class T> std::vector<T> column(const std::string& file,const std::string& attribute) {
    return std::get<std::vector<T>>(vdb::ReadPointAttribute(path(file),attribute).values);
  }
  std::filesystem::path directory; vdb::ProcessingOptions processing;
};

TEST_F(OpenVdbPointAttributes, ForeignProducerCountTypeIsReplacedFromActualNativeOffsets) {
  auto source=read("input.vdb");source->removeMeta("container:point_count");
  source->insertMeta("container:point_count",openvdb::StringMetadata("foreign cached count"));
  write("foreign-count.vdb",source);const auto before=bytes(path("foreign-count.vdb"));
  const auto schema=vdb::WritePointAttributeEdits(path("foreign-count.vdb"),path("count-fixed.vdb"),{}, {},{},processing);
  EXPECT_EQ(schema.pointCount,4u);const auto actual=read("count-fixed.vdb","edited");
  EXPECT_EQ(actual->metaValue<int64_t>("container:point_count"),4);
  EXPECT_EQ(actual->metaValue<std::string>("custom:grid"),"preserve grid metadata");
  EXPECT_EQ(bytes(path("foreign-count.vdb")),before);
}

TEST_F(OpenVdbPointAttributes, TinyPersistedScaleMapReadsAndEditsWithoutSimplifyingItsTypeOrCoefficients) {
  auto grid=read("input.vdb");
  std::stringstream record(std::ios::binary|std::ios::in|std::ios::out);
  openvdb::writeString(record,openvdb::math::UniformScaleTranslateMap::mapType());
  const openvdb::Vec3d origin(1e9+.125,-1e9,3);
  origin.write(record);
  for (const double n:{1e-6,1e-6,1e6,1e12,5e5}) openvdb::Vec3d(n).write(record);
  record.seekg(0); openvdb::io::setCurrentVersion(record);
  auto transform=std::make_shared<openvdb::math::Transform>(); transform->read(record);
  grid->setTransform(transform);
  {openvdb::io::File output(path("tiny.vdb").string()); output.write({std::make_shared<ExactPointFixture>(*grid)});}
  const auto original=bytes(path("tiny.vdb"));
  const auto schema=vdb::InspectPointAttributes(path("tiny.vdb"));
  EXPECT_EQ(schema.pointCount,4u);
  const auto tinyPositions=column<std::array<float,3>>("tiny.vdb","P");
  const auto originalPositions=column<std::array<float,3>>("input.vdb","P");
  EXPECT_EQ(tinyPositions,originalPositions);
  EXPECT_EQ(column<double>("tiny.vdb","pscale"),column<double>("input.vdb","pscale"));
  EXPECT_EQ(vdb::ReadPointGroup(path("tiny.vdb"),"keep"),(std::vector<uint8_t>{1,1,1,1}));
  const vdb::PointAttributeEdit edit{vdb::PointEditOperation::Add,"new_value",{},std::vector<int64_t>{INT64_MAX}};
  (void)vdb::WritePointAttributeEdits(path("tiny.vdb"),path("tiny-edited.vdb"),std::span(&edit,1),{}, {},processing);
  const auto output=read("tiny-edited.vdb","edited");
  EXPECT_EQ(output->transform().mapType(),transform->mapType());
  EXPECT_EQ(output->transform().baseMap()->applyMap(openvdb::Vec3d(0)),origin);
  for(size_t axis=0;axis<3;++axis){openvdb::Vec3d e(0);e[axis]=1;EXPECT_EQ(output->transform().baseMap()->applyJacobian(e),transform->baseMap()->applyJacobian(e));}
  EXPECT_EQ(column<int64_t>("tiny-edited.vdb","new_value"),(std::vector<int64_t>{INT64_MAX,INT64_MAX,INT64_MAX,INT64_MAX}));
  EXPECT_EQ(column<double>("tiny-edited.vdb","pscale"),column<double>("tiny.vdb","pscale"));
  EXPECT_EQ(output->metaValue<std::string>("custom:grid"),"preserve grid metadata");
  EXPECT_EQ(bytes(path("tiny.vdb")),original);
}

TEST_F(OpenVdbPointAttributes, FileMetadataSharesTheCatalogAndDecodedPointBudget) {
  const auto grid=read("input.vdb"); openvdb::MetaMap metadata;
  metadata.insertMeta("large:file",openvdb::StringMetadata(std::string(1024*1024,'m')));
  {openvdb::io::File output(path("file-meta.vdb").string());output.write({grid},metadata);}
  const auto original=bytes(path("file-meta.vdb"));
  auto cap=processing;cap.maxInputBytes=10*1024*1024;
  EXPECT_THROW((void)vdb::InspectPointAttributes(path("file-meta.vdb"),{},cap),std::invalid_argument);
  EXPECT_EQ(vdb::InspectPointAttributes(path("file-meta.vdb")).pointCount,4u);
  EXPECT_EQ(bytes(path("file-meta.vdb")),original);
}

TEST_F(OpenVdbPointAttributes, RawRecordOrderKeepsTheExistingNativeDefaultDescriptorPolicy) {
  auto higher=read("input.vdb"),lower=read("input.vdb");
  higher->setName("z-last");lower->setName("a-first");
  higher->removeMeta(openvdb::GridBase::META_FILE_MEM_BYTES);
  {openvdb::io::File output(path("record-order.vdb").string());output.setGridStatsMetadataEnabled(false);output.write({higher,lower});}
  EXPECT_EQ(vdb::InspectPointAttributes(path("record-order.vdb")).gridName,"a-first");
  EXPECT_EQ(vdb::InspectPointAttributes(path("record-order.vdb"),"descriptor:a-first").pointCount,4u);
  EXPECT_THROW((void)vdb::InspectPointAttributes(path("record-order.vdb"),"descriptor:z-last"),std::invalid_argument);
}

TEST_F(OpenVdbPointAttributes, NativeOrderIncludesInactiveVoxelPointsAndReadableFlags) {
  const auto schema=vdb::InspectPointAttributes(path("input.vdb"));
  EXPECT_EQ(schema.gridName,"points"); EXPECT_EQ(schema.pointCount,4u); EXPECT_EQ(schema.leafCount,3u);
  EXPECT_EQ(column<double>("input.vdb","pscale"),(std::vector<double>{16,1,2,32}));
  EXPECT_EQ(vdb::ReadPointGroup(path("input.vdb"),"keep"),(std::vector<uint8_t>{1,1,1,1}));
  const auto velocity=std::find_if(schema.attributes.begin(),schema.attributes.end(),[](const auto& a){return a.name=="v";});
  ASSERT_NE(velocity,schema.attributes.end()); EXPECT_TRUE(velocity->anyHidden); EXPECT_TRUE(velocity->readable);
}
TEST_F(OpenVdbPointAttributes, NumericAssignmentsRetainMixedSignedZeroBits) {
  const std::array<vdb::PointAttributeEdit,3> edits{{{Op::Add,"zeros",{},std::vector<double>{0,+0.0,-0.0,0}},
    {Op::Add,"vector_zeros",{},std::vector<std::array<float,3>>{{0,0,0},{+0.0f,0,0},{-0.0f,0,0},{0,0,0}}},
    {Op::Set,"pscale",{},std::vector<double>{0,+0.0,-0.0,0}}}};
  (void)vdb::WritePointAttributeEdits(path("input.vdb"),path("zeros.vdb"),edits,{}, {},processing);
  for(const auto* name:{"zeros","pscale"}){const auto values=column<double>("zeros.vdb",name);EXPECT_EQ(std::bit_cast<uint64_t>(values[1]),std::bit_cast<uint64_t>(+0.0));EXPECT_EQ(std::bit_cast<uint64_t>(values[2]),std::bit_cast<uint64_t>(-0.0));}
  const auto vectors=column<std::array<float,3>>("zeros.vdb","vector_zeros");EXPECT_EQ(std::bit_cast<uint32_t>(vectors[1][0]),std::bit_cast<uint32_t>(+0.0f));EXPECT_EQ(std::bit_cast<uint32_t>(vectors[2][0]),std::bit_cast<uint32_t>(-0.0f));
}
TEST_F(OpenVdbPointAttributes, ReferencedAndOrphanStridedGroupBackingRejectBeforeNativeHandles) {
  auto grid=read("input.vdb");
  for (auto leaf=grid->tree().beginLeaf();leaf;++leaf) {
    const auto backing=leaf->attributeSet().groupIndex("keep").first;
    auto attributes=std::make_unique<points::AttributeSet>(leaf->attributeSet());
    attributes->replace(backing,std::make_shared<points::GroupAttributeArray>(openvdb::Index(leaf->pointCount()),2,true));
    leaf->replaceAttributeSet(attributes.release());
  }
  write("strided-group.vdb",grid);const auto original=bytes(path("strided-group.vdb"));
  EXPECT_THROW((void)vdb::InspectPointAttributes(path("strided-group.vdb")),std::invalid_argument);
  EXPECT_THROW((void)vdb::ReadPointGroup(path("strided-group.vdb"),"keep"),std::invalid_argument);
  const std::array<vdb::PointGroupEdit,1> set{{{Op::Set,"keep",{},{1}}}};
  EXPECT_THROW((void)vdb::WritePointAttributeEdits(path("strided-group.vdb"),path("group-set.vdb"),{},set,{},processing),std::invalid_argument);
  EXPECT_FALSE(std::filesystem::exists(path("group-set.vdb")));EXPECT_EQ(bytes(path("strided-group.vdb")),original);
  points::dropGroup(grid->tree(),"keep",false);write("orphan-group.vdb",grid);
  EXPECT_NO_THROW((void)vdb::InspectPointAttributes(path("orphan-group.vdb")));
  const std::array<vdb::PointGroupEdit,1> add{{{Op::Add,"new-group",{},{1}}}};
  EXPECT_THROW((void)vdb::WritePointAttributeEdits(path("orphan-group.vdb"),path("group-add.vdb"),{},add,{},processing),std::invalid_argument);
  EXPECT_FALSE(std::filesystem::exists(path("group-add.vdb")));
}

TEST_F(OpenVdbPointAttributes, AllEightNativeTypesRoundTripThroughGenuineArraysAndStringDictionary) {
  const std::array<vdb::PointAttributeEdit,8> edits{{
    {Op::Add,"flag",{},std::vector<uint8_t>{0,1,0,1}},
    {Op::Add,"i32",{},std::vector<int32_t>{-5,0,7,99}},
    {Op::Add,"i64",{},std::vector<int64_t>{INT64_MIN,0,INT64_MAX,42}},
    {Op::Add,"f32",{},std::vector<float>{1.25f}},
    {Op::Add,"f64",{},std::vector<double>{-1e300,1e-300,0,9}},
    {Op::Add,"v32",{},std::vector<std::array<float,3>>{{1,2,3}}},
    {Op::Add,"v64",{},std::vector<std::array<double,3>>{{-1,2,3},{4,5,6},{7,8,9},{10,11,12}}},
    {Op::Add,"label",{},std::vector<std::string>{"red",std::string("\xe8\x93\x9d"),"red",std::string("a\0b",3)}}}};
  const auto before=bytes(path("input.vdb"));
  const auto result=vdb::WritePointAttributeEdits(path("input.vdb"),path("typed.vdb"),edits,{}, {},processing);
  EXPECT_EQ(result.gridName,"edited"); EXPECT_EQ(result.pointCount,4u);
  EXPECT_EQ(column<uint8_t>("typed.vdb","flag"),(std::vector<uint8_t>{0,1,0,1}));
  EXPECT_EQ(column<int32_t>("typed.vdb","i32"),(std::vector<int32_t>{-5,0,7,99}));
  EXPECT_EQ(column<int64_t>("typed.vdb","i64"),(std::vector<int64_t>{INT64_MIN,0,INT64_MAX,42}));
  EXPECT_EQ(column<float>("typed.vdb","f32"),(std::vector<float>{1.25f,1.25f,1.25f,1.25f}));
  EXPECT_EQ(column<double>("typed.vdb","f64"),(std::vector<double>{-1e300,1e-300,0,9}));
  EXPECT_EQ((column<std::array<float,3>>("typed.vdb","v32")),(std::vector<std::array<float,3>>(4,{1,2,3})));
  EXPECT_EQ((column<std::array<double,3>>("typed.vdb","v64")),(std::get<std::vector<std::array<double,3>>>(edits[6].values)));
  EXPECT_EQ(column<std::string>("typed.vdb","label"),std::get<std::vector<std::string>>(edits[7].values));
  auto native=read("typed.vdb","edited"); const auto& leaf=*native->tree().cbeginLeaf();
  EXPECT_TRUE(leaf.constAttributeArray("label").isType<points::StringAttributeArray>());
  EXPECT_TRUE(leaf.attributeSet().descriptor().getMetadata().beginMeta()!=leaf.attributeSet().descriptor().getMetadata().endMeta());
  EXPECT_EQ(bytes(path("input.vdb")),before);
}

TEST_F(OpenVdbPointAttributes, SequentialRenameSetDropAndGroupsPreserveUnselectedBits) {
  const std::array<vdb::PointAttributeEdit,4> attributes{{
    {Op::Add,"temporary",{},std::vector<double>{3}}, {Op::Rename,"temporary","mass"},
    {Op::Set,"mass",{},std::vector<double>{1,2,3,4}}, {Op::Set,"pscale",{},std::vector<double>{2.5}}}};
  const std::array<vdb::PointGroupEdit,3> groups{{{Op::Add,"selected",{},{1,0,1,0}},
    {Op::Rename,"selected","chosen"}, {Op::Add,"all",{},{1}}}};
  (void)vdb::WritePointAttributeEdits(path("input.vdb"),path("groups.vdb"),attributes,groups,{},processing);
  EXPECT_EQ(column<double>("groups.vdb","mass"),(std::vector<double>{1,2,3,4}));
  EXPECT_EQ(column<double>("groups.vdb","pscale"),(std::vector<double>{2.5,2.5,2.5,2.5}));
  EXPECT_EQ(vdb::ReadPointGroup(path("groups.vdb"),"chosen"),(std::vector<uint8_t>{1,0,1,0}));
  EXPECT_EQ(vdb::ReadPointGroup(path("groups.vdb"),"keep"),(std::vector<uint8_t>{1,1,1,1}));
  const std::array<vdb::PointAttributeEdit,1> drop{{{Op::Drop,"mass"}}};
  const std::array<vdb::PointGroupEdit,2> changes{{{Op::Drop,"chosen"},{Op::Set,"all",{},{0,1,0,1}}}};
  (void)vdb::WritePointAttributeEdits(path("groups.vdb"),path("dropped.vdb"),drop,changes,{},processing);
  EXPECT_THROW((void)vdb::ReadPointAttribute(path("dropped.vdb"),"mass"),std::invalid_argument);
  EXPECT_THROW((void)vdb::ReadPointGroup(path("dropped.vdb"),"chosen"),std::invalid_argument);
  EXPECT_EQ(vdb::ReadPointGroup(path("dropped.vdb"),"keep"),(std::vector<uint8_t>{1,1,1,1}));
  EXPECT_EQ(vdb::ReadPointGroup(path("dropped.vdb"),"all"),(std::vector<uint8_t>{0,1,0,1}));
}

TEST_F(OpenVdbPointAttributes, TransformOffsetsMetadataHiddenFlagsAndUnsupportedCodecArePreserved) {
  using Packed=points::TypedAttributeArray<float,points::TruncateCodec>;
  if (!Packed::isRegistered()) Packed::registerType();
  auto source=read("input.vdb"); points::appendAttribute<float,points::TruncateCodec>(source->tree(),"packed",.75f);
  source->transform().postRotate(.4,openvdb::math::Z_AXIS); source->transform().postTranslate({1e6+.125,2,3});
  write("packed.vdb",source);
  const std::array<vdb::PointAttributeEdit,1> edits{{{Op::Add,"mass",{},std::vector<double>{1}}}};
  (void)vdb::WritePointAttributeEdits(path("packed.vdb"),path("preserved.vdb"),edits,{}, {},processing);
  const auto target=read("preserved.vdb","edited"); EXPECT_EQ(source->transform(),target->transform());
  EXPECT_TRUE(source->tree().hasSameTopology(target->tree()));
  EXPECT_EQ(target->metaValue<std::string>("custom:grid"),"preserve grid metadata");
  for (auto leaf=source->tree().cbeginLeaf();leaf;++leaf) {
    const auto* changed=target->tree().probeConstLeaf(leaf->origin()); ASSERT_NE(changed,nullptr);
    EXPECT_EQ(changed->attributeSet().descriptor().getMetadata().metaValue<std::string>("custom:descriptor"),"preserve dictionary metadata");
    EXPECT_TRUE(changed->constAttributeArray("v").isHidden()); EXPECT_TRUE(changed->constAttributeArray("packed").isType<Packed>());
    points::AttributeHandle<float,points::TruncateCodec> handle(changed->constAttributeArray("packed"));
    for (openvdb::Index n=0;n<changed->pointCount();++n) EXPECT_EQ(handle.get(n),.75f);
    for (openvdb::Index n=0;n<Grid::TreeType::LeafNodeType::NUM_VALUES;++n) EXPECT_EQ(leaf->getValue(n),changed->getValue(n));
  }
  EXPECT_THROW((void)vdb::ReadPointAttribute(path("preserved.vdb"),"packed"),std::invalid_argument);
}

TEST_F(OpenVdbPointAttributes, PositionIsProtectedFromEveryMutationAndTransactionLeavesNoOutput) {
  for (const auto operation:{Op::Add,Op::Set,Op::Rename,Op::Drop}) {
    vdb::PointAttributeEdit edit; edit.operation=operation; edit.name="P";
    if (operation==Op::Add || operation==Op::Set) edit.values=std::vector<std::array<float,3>>{{0,0,0}};
    if (operation==Op::Rename) edit.newName="other";
    try {(void)vdb::WritePointAttributeEdits(path("input.vdb"),path("forbidden.vdb"),std::span(&edit,1),{}, {},processing); FAIL();}
    catch (const std::invalid_argument& error) {EXPECT_NE(std::string(error.what()).find("P is protected"),std::string::npos);}
  }
  const std::array<vdb::PointAttributeEdit,2> edits{{{Op::Add,"valid",{},std::vector<double>{1}}, {Op::Rename,"v","P"}}};
  EXPECT_THROW((void)vdb::WritePointAttributeEdits(path("input.vdb"),path("forbidden.vdb"),edits,{}, {},processing),std::invalid_argument);
  EXPECT_FALSE(std::filesystem::exists(path("forbidden.vdb")));
}

TEST_F(OpenVdbPointAttributes, InvalidPayloadTypeShapeNamesAndGroupStorageAreRejected) {
  const std::array<vdb::PointAttributeEdit,6> invalid{{
    {Op::Set,"pscale",{},std::vector<float>{1}}, {Op::Add,"bad",{},std::vector<double>{1,2}},
    {Op::Add,"flag",{},std::vector<uint8_t>{2}}, {Op::Add,"nan",{},std::vector<double>{NAN}},
    {Op::Add,"v",{},std::vector<double>{1}}, {Op::Drop,"__group"}}};
  for (const auto& edit:invalid) EXPECT_THROW((void)vdb::WritePointAttributeEdits(path("input.vdb"),path("bad.vdb"),std::span(&edit,1),{}, {},processing),std::invalid_argument);
  const std::array<vdb::PointGroupEdit,1> group{{{Op::Set,"keep",{},{1,2,0,1}}}};
  EXPECT_THROW((void)vdb::WritePointAttributeEdits(path("input.vdb"),path("bad.vdb"),{},group,{},processing),std::invalid_argument);
  EXPECT_FALSE(std::filesystem::exists(path("bad.vdb")));
}

TEST_F(OpenVdbPointAttributes, GroupBackingCapacityUsesUnusedBitsAndPreservesInactiveMembership) {
  const auto before=vdb::InspectPointAttributes(path("input.vdb"));
  std::vector<vdb::PointGroupEdit> groups;
  for (int n=0;n<7;++n) groups.push_back({Op::Add,"group"+std::to_string(n),{},{uint8_t(n%2)}});
  vdb::PointAttributeOptions o; o.maxAttributes=before.attributes.size();
  (void)vdb::WritePointAttributeEdits(path("input.vdb"),path("eight.vdb"),{},groups,{},processing,o);
  EXPECT_EQ(vdb::InspectPointAttributes(path("eight.vdb")).attributes.size(),before.attributes.size());
  groups.push_back({Op::Add,"overflow",{},{1}});
  EXPECT_THROW((void)vdb::WritePointAttributeEdits(path("input.vdb"),path("nine.vdb"),{},groups,{},processing,o),std::invalid_argument);
  o.maxAttributes+=1;
  (void)vdb::WritePointAttributeEdits(path("input.vdb"),path("nine.vdb"),{},groups,{},processing,o);
  EXPECT_EQ(vdb::ReadPointGroup(path("nine.vdb"),"overflow"),(std::vector<uint8_t>{1,1,1,1}));
  EXPECT_EQ(vdb::ReadPointGroup(path("nine.vdb"),"keep"),(std::vector<uint8_t>{1,1,1,1}));
}

TEST_F(OpenVdbPointAttributes, StringCountMemoryLimitsAndExistingOutputAreEnforced) {
  const std::array<vdb::PointAttributeEdit,1> text{{{Op::Add,"label",{},std::vector<std::string>{"long label"}}}};
  vdb::PointAttributeOptions o; o.maxStringBytes=3;
  EXPECT_THROW((void)vdb::WritePointAttributeEdits(path("input.vdb"),path("bad.vdb"),text,{}, {},processing,o),std::invalid_argument);
  o={}; o.maxPoints=3; EXPECT_THROW((void)vdb::InspectPointAttributes(path("input.vdb"),{},processing,o),std::invalid_argument);
  o={}; o.maxEdits=1; const std::array<vdb::PointAttributeEdit,2> many{{text[0],{Op::Add,"other",{},std::vector<double>{1}}}};
  EXPECT_THROW((void)vdb::WritePointAttributeEdits(path("input.vdb"),path("bad.vdb"),many,{}, {},processing,o),std::invalid_argument);
  auto p=processing; p.maxInputBytes=5*1024*1024;
  EXPECT_THROW((void)vdb::WritePointAttributeEdits(path("input.vdb"),path("bad.vdb"),text,{}, {},p),std::invalid_argument);
  {std::ofstream output(path("existing.vdb")); output<<"preserve";} const auto original=bytes(path("existing.vdb"));
  EXPECT_THROW((void)vdb::WritePointAttributeEdits(path("input.vdb"),path("existing.vdb"),text,{}, {},processing),std::invalid_argument);
  EXPECT_EQ(bytes(path("existing.vdb")),original);
}

TEST_F(OpenVdbPointAttributes, EmptyTreesCanBeCopiedButCannotPersistNewSchema) {
  auto p=processing; p.gridName="points"; (void)vdb::WritePoints({},path("empty.vdb"),p);
  const auto schema=vdb::WritePointAttributeEdits(path("empty.vdb"),path("copy.vdb"),{}, {},{},processing);
  EXPECT_EQ(schema.pointCount,0u); EXPECT_TRUE(schema.attributes.empty());
  const std::array<vdb::PointAttributeEdit,1> edit{{{Op::Add,"value",{},std::vector<double>{1}}}};
  EXPECT_THROW((void)vdb::WritePointAttributeEdits(path("empty.vdb"),path("not-written.vdb"),edit,{}, {},processing),std::invalid_argument);
  EXPECT_FALSE(std::filesystem::exists(path("not-written.vdb")));
}

TEST_F(OpenVdbPointAttributes, HeterogeneousDescriptorMetadataIsRejectedInsteadOfReplaced) {
  auto grid=read("input.vdb"); auto leaf=grid->tree().beginLeaf();
  auto descriptor=std::make_shared<points::AttributeSet::Descriptor>(leaf->attributeSet().descriptor());
  descriptor->getMetadata().insertMeta("leaf-only",openvdb::StringMetadata("do not lose"));
  auto* attributes=new points::AttributeSet(leaf->attributeSet()); attributes->resetDescriptor(descriptor,true); leaf->replaceAttributeSet(attributes,true);
  write("heterogeneous.vdb",grid);
  EXPECT_THROW((void)vdb::InspectPointAttributes(path("heterogeneous.vdb")),std::invalid_argument);
  const std::array<vdb::PointAttributeEdit,1> edit{{{Op::Add,"value",{},std::vector<double>{1}}}};
  EXPECT_THROW((void)vdb::WritePointAttributeEdits(path("heterogeneous.vdb"),path("not-written.vdb"),edit,{}, {},processing),std::invalid_argument);
}

TEST_F(OpenVdbPointAttributes, VariableStrideDataIsPreservedButNotReinterpretedAsScalarColumns) {
  auto source=read("input.vdb"); points::appendAttribute<double>(source->tree(),"pair",.25,2,true);
  write("strided.vdb",source);
  const std::array<vdb::PointAttributeEdit,1> edit{{{Op::Add,"mass",{},std::vector<double>{1}}}};
  (void)vdb::WritePointAttributeEdits(path("strided.vdb"),path("preserved.vdb"),edit,{}, {},processing);
  const auto target=read("preserved.vdb","edited");
  for (auto leaf=target->tree().cbeginLeaf();leaf;++leaf) {
    const auto& array=leaf->constAttributeArray("pair"); EXPECT_EQ(array.stride(),2);
    points::AttributeHandle<double> handle(array);
    for (openvdb::Index n=0;n<leaf->pointCount();++n) {EXPECT_EQ(handle.get(n,0),.25); EXPECT_EQ(handle.get(n,1),.25);}
  }
  EXPECT_THROW((void)vdb::ReadPointAttribute(path("preserved.vdb"),"pair"),std::invalid_argument);
  const std::array<vdb::PointAttributeEdit,1> set{{{Op::Set,"pair",{},std::vector<double>{1}}}};
  EXPECT_THROW((void)vdb::WritePointAttributeEdits(path("strided.vdb"),path("bad.vdb"),set,{}, {},processing),std::invalid_argument);
}

TEST_F(OpenVdbPointAttributes, DictionaryAndDecodedStringBudgetsAreCheckedSeparately) {
  const std::array<vdb::PointAttributeEdit,1> edit{{{Op::Add,"text",{},std::vector<std::string>{std::string(100,'x')}}}};
  (void)vdb::WritePointAttributeEdits(path("input.vdb"),path("text.vdb"),edit,{}, {},processing);
  vdb::PointAttributeOptions o; o.maxStringBytes=200;
  EXPECT_EQ(vdb::InspectPointAttributes(path("text.vdb"),{},processing,o).pointCount,4u);
  EXPECT_THROW((void)vdb::ReadPointAttribute(path("text.vdb"),"text",{},processing,o),std::invalid_argument);
  o.maxStringBytes=400;
  EXPECT_EQ(std::get<std::vector<std::string>>(vdb::ReadPointAttribute(path("text.vdb"),"text",{},processing,o).values).size(),4u);
  auto source=read("input.vdb"); source->insertMeta("large",openvdb::StringMetadata(std::string(8*1024*1024,'x'))); write("metadata.vdb",source);
  auto p=processing; p.maxInputBytes=128*1024*1024;
  EXPECT_THROW((void)vdb::WritePointAttributeEdits(path("metadata.vdb"),path("bad.vdb"),edit,{}, {},p),std::invalid_argument);
  EXPECT_FALSE(std::filesystem::exists(path("bad.vdb")));
}

TEST_F(OpenVdbPointAttributes, MissingStatisticsAliasedGroupsAndDisabledDelayLoadingAreExplicit) {
  auto source=read("input.vdb");
  source->removeMeta(openvdb::GridBase::META_FILE_MEM_BYTES); source->removeMeta(openvdb::GridBase::META_FILE_VOXEL_COUNT);
  {openvdb::io::File file(path("nostats.vdb").string()); file.setGridStatsMetadataEnabled(false); file.write({source});}
  EXPECT_THROW((void)vdb::InspectPointAttributes(path("nostats.vdb")),std::invalid_argument);
  auto descriptor=points::makeDescriptorUnique(source->tree()); descriptor->setGroup("alias",descriptor->groupOffset("keep")); write("alias.vdb",source);
  EXPECT_THROW((void)vdb::InspectPointAttributes(path("alias.vdb")),std::invalid_argument);
  struct Environment {
    bool present=false; std::string previous;
    Environment() {if (const auto* value=std::getenv("OPENVDB_DISABLE_DELAYED_LOAD")) {present=true; previous=value;}
#ifdef _WIN32
      _putenv_s("OPENVDB_DISABLE_DELAYED_LOAD","1");
#else
      setenv("OPENVDB_DISABLE_DELAYED_LOAD","1",1);
#endif
    }
    ~Environment() {
#ifdef _WIN32
      _putenv_s("OPENVDB_DISABLE_DELAYED_LOAD",present?previous.c_str():"");
#else
      if (present) setenv("OPENVDB_DISABLE_DELAYED_LOAD",previous.c_str(),1); else unsetenv("OPENVDB_DISABLE_DELAYED_LOAD");
#endif
    }
  } environment;
  try {(void)vdb::InspectPointAttributes(path("input.vdb")); FAIL();}
  catch (const std::invalid_argument& error) {EXPECT_NE(std::string(error.what()).find("unset OPENVDB_DISABLE_DELAYED_LOAD"),std::string::npos);}
}

TEST_F(OpenVdbPointAttributes, DuplicateDisplayNamesNeedDescriptorsAndCatalogCollisionsFailBeforeLookup) {
  auto a=read("input.vdb"),b=a->deepCopy(); b->insertMeta("custom:grid",openvdb::StringMetadata("second"));
  {openvdb::io::File file(path("duplicate.vdb").string()); file.write({a,b});}
  EXPECT_THROW((void)vdb::InspectPointAttributes(path("duplicate.vdb"),"points"),std::invalid_argument);
  const std::array<vdb::PointAttributeEdit,1> edit{{{Op::Add,"value",{},std::vector<double>{1}}}};
  (void)vdb::WritePointAttributeEdits(path("duplicate.vdb"),path("selected.vdb"),edit,{},"descriptor:points[1]",processing);
  EXPECT_EQ(read("selected.vdb","edited")->metaValue<std::string>("custom:grid"),"second");
  auto literal=a->deepCopy(); literal->setName("points[0]");
  {openvdb::io::File file(path("collision.vdb").string()); file.write({a,b,literal});}
  for (const std::string selector:{std::string{},std::string("descriptor:points[0]")})
    EXPECT_THROW((void)vdb::InspectPointAttributes(path("collision.vdb"),selector),std::invalid_argument);
}
#endif
} // namespace
