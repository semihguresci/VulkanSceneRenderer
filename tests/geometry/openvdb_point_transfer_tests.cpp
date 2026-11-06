#include "Container/geometry/OpenVdbPointTransfer.h"
#include <gtest/gtest.h>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <map>
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
#include <openvdb/io/File.h>
#include <openvdb/points/PointAttribute.h>
#include <openvdb/points/PointConversion.h>
#include <openvdb/points/PointGroup.h>
#endif
namespace {
namespace vdb=container::geometry::vdb;
TEST(OpenVdbPointTransferAvailability, DisabledBeforeAllPathsAndOptions) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  GTEST_SKIP();
#else
  vdb::PointTransferOptions o;o.constantMass=-1;o.maxWork=0;vdb::ProcessingOptions p;p.maxInputBytes=0;
  try{(void)vdb::WriteTransferredPointAttribute("missing","missing/out","P",o,{},p);FAIL();}
  catch(const std::runtime_error& e){EXPECT_NE(std::string(e.what()).find("ENABLE_OPENVDB=ON"),std::string::npos);}
#endif
}
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
namespace points=openvdb::points;
using Grid=points::PointDataGrid;
using V=openvdb::Vec3d;
using C=openvdb::Coord;
struct Input {V p;double value,mass;V vector;bool selected=true;};
class OpenVdbPointTransfer:public ::testing::Test {
protected:
  void SetUp()override {
    openvdb::initialize();reg<bool>();reg<int32_t>();reg<int64_t>();reg<float>();reg<double>();reg<openvdb::Vec3i>();reg<openvdb::Vec3f>();reg<openvdb::Vec3d>();
    using Encoded=points::TypedAttributeArray<float,points::TruncateCodec>;if(!Encoded::isRegistered())Encoded::registerType();
    static std::atomic_uint64_t serial{};directory=std::filesystem::temp_directory_path()/("container-point-transfer-"+
      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+"-"+std::to_string(++serial));std::filesystem::create_directory(directory);
    p.gridName="field";o.targetIndexToWorld={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    records={{{-.25,.25,.5},2,1,{2,-4,6}},{{.25,.25,.5},4,3,{4,-8,12}},{{8,0,0},8,2,{8,-16,24},false}};
    write("source.vdb",create(records));
  }
  template<class T>void reg(){using A=points::TypedAttributeArray<T>;if(!A::isRegistered())A::registerType();}
  void TearDown()override {std::error_code e;for(const auto& f:std::filesystem::directory_iterator(directory))std::filesystem::remove_all(f.path(),e);std::filesystem::remove(directory,e);}
  auto path(const std::string& n)const{return directory/n;}
  std::string bytes(const std::filesystem::path& name){std::ifstream file(name,std::ios::binary);return {std::istreambuf_iterator<char>(file),{}};}
  Grid::Ptr create(const std::vector<Input>& input) {
    std::vector<V> positions;for(const auto& r:input)positions.push_back(r.p);
    auto unit=openvdb::math::Transform::createLinearTransform(1);points::PointAttributeVector<V> wrapper(positions);
    auto indices=openvdb::tools::createPointIndexGrid<openvdb::tools::PointIndexGrid>(wrapper,*unit);
    auto grid=points::createPointDataGrid<points::NullCodec,Grid>(*indices,wrapper,*unit);grid->setName("points");
    points::appendAttribute<double>(grid->tree(),"value");points::appendAttribute<double>(grid->tree(),"mass");points::appendAttribute<openvdb::Vec3d>(grid->tree(),"vector");
    points::appendAttribute<int64_t>(grid->tree(),"ordinal");points::appendAttribute<int32_t>(grid->tree(),"unrelated_dynamic",7,20,false);
    points::appendAttribute<float,points::TruncateCodec>(grid->tree(),"encoded",.375f);
    points::appendAttribute<bool>(grid->tree(),"false_value",false);points::appendAttribute<int32_t>(grid->tree(),"int32_value",INT32_MIN);
    points::appendAttribute<int64_t>(grid->tree(),"int64_value",INT64_MIN);points::appendAttribute<openvdb::Vec3i>(grid->tree(),"int_vector",{2,-3,4});
    points::appendAttribute<openvdb::Vec3f>(grid->tree(),"float_vector",{.25f,-.5f,1});
    points::appendGroup(grid->tree(),"A");points::appendGroup(grid->tree(),"empty");
    std::vector<double> values,mass;std::vector<V> vectors;std::vector<int64_t> ids;
    for(size_t n=0;n<input.size();++n){values.push_back(input[n].value);mass.push_back(input[n].mass);vectors.push_back(input[n].vector);ids.push_back(int64_t(n));}
    points::PointAttributeVector<double> v(values),m(mass);points::PointAttributeVector<V> vv(vectors);points::PointAttributeVector<int64_t> ii(ids);
    points::populateAttribute(grid->tree(),indices->tree(),"value",v);points::populateAttribute(grid->tree(),indices->tree(),"mass",m);points::populateAttribute(grid->tree(),indices->tree(),"vector",vv);points::populateAttribute(grid->tree(),indices->tree(),"ordinal",ii);
    for(auto leaf=grid->tree().beginLeaf();leaf;++leaf){points::AttributeHandle<int64_t> ids(leaf->constAttributeArray("ordinal"));auto a=leaf->groupWriteHandle("A");for(auto i=leaf->beginIndexAll();i;++i)a.set(*i,input[size_t(ids.get(*i))].selected);leaf->attributeArray("value").setHidden(true);}
    points::makeDescriptorUnique(grid->tree())->getMetadata().insertMeta("custom:descriptor",openvdb::StringMetadata("unchanged private columns"));
    grid->insertMeta("artist",openvdb::StringMetadata(std::string("a\0b",3)));return grid;
  }
  void write(const std::string& name,Grid::ConstPtr grid) {
    openvdb::MetaMap meta;meta.insertMeta("file:id",openvdb::Int64Metadata(INT64_C(9007199254740993)));
    openvdb::io::File file(path(name).string());file.setCompression(openvdb::io::COMPRESS_ZIP);file.write({grid},meta);file.close();
  }
  template<class T>typename T::Ptr read(const std::string& name,const std::string& grid="field") {
    openvdb::io::File file(path(name).string());file.open(false);auto result=openvdb::gridPtrCast<T>(file.readGrid(grid));file.close();return result;
  }
  auto run(const std::string& attr="value",const std::string& input="source.vdb",const std::string& output="out.vdb") {return vdb::WriteTransferredPointAttribute(path(input),path(output),attr,o,{},p,limits);}
  std::map<C,std::pair<double,double>> oracle(const std::vector<Input>& input,bool selectedOnly=false) {
    std::map<C,std::pair<double,double>> cells;
    for(const auto& r:input)if(!selectedOnly || r.selected) {
      const C base=C::floor(r.p);const V f=r.p-base.asVec3d();
      for(int x=0;x<2;++x)for(int y=0;y<2;++y)for(int z=0;z<2;++z) {
        const double w=(x?f[0]:1-f[0])*(y?f[1]:1-f[1])*(z?f[2]:1-f[2])*r.mass;if(w==0)continue;
        auto& c=cells[base+C(x,y,z)];c.first+=w*r.value;c.second+=w;
      }
    }
    return cells;
  }
  std::filesystem::path directory;vdb::PointTransferOptions o;vdb::ProcessingOptions p;vdb::PointAttributeOptions limits;std::vector<Input> records;
};

TEST_F(OpenVdbPointTransfer, WeightedMeanAndSumMatchIndependentEightCellMassOracles) {
  const auto original=bytes(path("source.vdb"));o.weightAttribute="mass";
  const auto result=run();EXPECT_EQ(result.inputCount,3);EXPECT_EQ(result.selectedCount,3);EXPECT_EQ(result.componentCount,1);
  EXPECT_DOUBLE_EQ(result.totalMass,6);EXPECT_DOUBLE_EQ(result.depositedMass,6);EXPECT_DOUBLE_EQ(result.massErrorUpperBound,0);
  auto field=read<openvdb::DoubleGrid>("out.vdb"),weight=read<openvdb::DoubleGrid>("out.vdb","field_weight");const auto expected=oracle(records);
  EXPECT_EQ(field->activeVoxelCount(),expected.size());EXPECT_EQ(weight->activeVoxelCount(),expected.size());
  for(const auto& [c,pair]:expected){EXPECT_DOUBLE_EQ(field->tree().getValue(c),pair.first/pair.second);EXPECT_DOUBLE_EQ(weight->tree().getValue(c),pair.second);}
  EXPECT_EQ(field->background(),0);EXPECT_EQ(weight->background(),0);EXPECT_EQ(field->getGridClass(),openvdb::GRID_UNKNOWN);
  o.mode=vdb::PointTransferMode::WeightedSum;const auto sum=run("value","source.vdb","sum.vdb");auto sums=read<openvdb::DoubleGrid>("sum.vdb");
  for(const auto& [c,pair]:expected)EXPECT_DOUBLE_EQ(sums->tree().getValue(c),pair.first);
  EXPECT_EQ(sum.totalMass,result.totalMass);EXPECT_EQ(original,bytes(path("source.vdb")));
}
TEST_F(OpenVdbPointTransfer, SelectedGroupsIncludeInactiveRecordsAndIgnoreUnselectedNonfiniteFields) {
  auto grid=create(records);grid->tree().setActiveState({0,0,1},false);
  // Make an occupied source voxel inactive explicitly, using native point coords.
  for(auto leaf=grid->tree().beginLeaf();leaf;++leaf)for(auto i=leaf->beginIndexAll();i;++i)if(i.getCoord()[0]==0)leaf->setActiveState(i.getCoord(),false);
  write("inactive.vdb",grid);o.groups.anyOf={"A"};o.weightAttribute="mass";
  const auto result=run("value","inactive.vdb");EXPECT_EQ(result.selectedCount,2);EXPECT_GT(result.selectedInactiveCount,0);EXPECT_EQ(result.totalMass,4);
  const auto expected=oracle(records,true);auto field=read<openvdb::DoubleGrid>("out.vdb");for(const auto& [c,pair]:expected)EXPECT_DOUBLE_EQ(field->tree().getValue(c),pair.first/pair.second);
  auto special=records;special.back().value=NAN;special.back().mass=-1;write("unselected.vdb",create(special));
  EXPECT_NO_THROW((void)run("value","unselected.vdb","selected.vdb"));
  o.groups={};EXPECT_THROW(run("value","unselected.vdb","bad.vdb"),std::invalid_argument);EXPECT_FALSE(std::filesystem::exists(path("bad.vdb")));
}
TEST_F(OpenVdbPointTransfer, AllEightNumericFamiliesAndRuntimeCodecProduceRawStoredValues) {
  const std::array<std::pair<const char*,double>,5> scalar{{{"false_value",0},{"int32_value",double(INT32_MIN)},{"int64_value",double(INT64_MIN)},{"encoded",.375},{"value",2}}};
  auto one=records;one.resize(1);one[0].p={0,0,0};write("one.vdb",create(one));
  for(const auto& [name,value]:scalar){const auto output=std::string(name)+".vdb";const auto result=run(name,"one.vdb",output);EXPECT_EQ(result.componentCount,1);auto grid=read<openvdb::DoubleGrid>(output);EXPECT_DOUBLE_EQ(grid->tree().getValue({0,0,0}),value);EXPECT_TRUE(grid->tree().isValueOn({0,0,0}));}
  for(const auto& name:{"vector","int_vector","float_vector"}) {
    const auto output=std::string(name)+".vdb";const auto result=run(name,"one.vdb",output);EXPECT_EQ(result.componentCount,3);auto grid=read<openvdb::Vec3DGrid>(output);
    const V expected=std::string(name)=="vector"?V(2,-4,6):std::string(name)=="int_vector"?V(2,-3,4):V(.25,-.5,1);
    EXPECT_EQ(grid->tree().getValue({0,0,0}),expected);EXPECT_TRUE(grid->isInWorldSpace());EXPECT_EQ(grid->getVectorType(),openvdb::VEC_INVARIANT);
  }
}
TEST_F(OpenVdbPointTransfer, Int64ExactnessDefaultsToRejectAndExplicitRoundingIsCounted) {
  auto grid=create({{{0,0,0},1,1,{1,2,3}}});for(auto leaf=grid->tree().beginLeaf();leaf;++leaf){points::AttributeWriteHandle<int64_t> h(leaf->attributeArray("int64_value"));h.set(0,INT64_C(9007199254740993));}write("ids.vdb",grid);
  EXPECT_THROW(run("int64_value","ids.vdb"),std::invalid_argument);EXPECT_FALSE(std::filesystem::exists(path("out.vdb")));
  o.integers=vdb::PointTransferIntegers::AllowRounded;const auto result=run("int64_value","ids.vdb");EXPECT_EQ(result.roundedIntegerComponents,1);EXPECT_DOUBLE_EQ(read<openvdb::DoubleGrid>("out.vdb")->tree().getValue({0,0,0}),9007199254740992.);
}
TEST_F(OpenVdbPointTransfer, ExactCancellationAndPermutationAvoidIntermediateOverflowAndKeepPositiveSupport) {
  std::vector<Input> data{{{0,0,0},1e308,1e154,{}},{{0,0,0},-1e308,1e154,{}},{{0,0,0},1,1e154,{}}};write("huge.vdb",create(data));o.weightAttribute="mass";o.mode=vdb::PointTransferMode::WeightedSum;
  const auto result=run("value","huge.vdb");EXPECT_DOUBLE_EQ(read<openvdb::DoubleGrid>("out.vdb")->tree().getValue({0,0,0}),1e154);
  std::reverse(data.begin(),data.end());write("reversed.vdb",create(data));const auto reversed=run("value","reversed.vdb","reverse.vdb");
  EXPECT_DOUBLE_EQ(read<openvdb::DoubleGrid>("reverse.vdb")->tree().getValue({0,0,0}),1e154);EXPECT_EQ(result.depositedMass,reversed.depositedMass);
  data.pop_back();data[0].value=1e308;data[1].value=-1e308;write("cancel.vdb",create(data));const auto cancelled=run("value","cancel.vdb","cancelled.vdb");auto zero=read<openvdb::DoubleGrid>("cancelled.vdb");EXPECT_EQ(zero->tree().getValue({0,0,0}),0);EXPECT_TRUE(zero->tree().isValueOn({0,0,0}));EXPECT_EQ(cancelled.field.activeVoxelCount,1);
}
TEST_F(OpenVdbPointTransfer, TinyProductsSurviveInMeanAndCancellationButUnrepresentableNativeOutputsReject) {
  std::vector<Input> data{{{0,0,0},1e-300,1e-300,{}}};write("tiny.vdb",create(data));o.weightAttribute="mass";
  const auto result=run("value","tiny.vdb");EXPECT_DOUBLE_EQ(read<openvdb::DoubleGrid>("out.vdb")->tree().getValue({0,0,0}),1e-300);EXPECT_DOUBLE_EQ(result.totalMass,1e-300);
  o.mode=vdb::PointTransferMode::WeightedSum;EXPECT_THROW(run("value","tiny.vdb","underflow.vdb"),std::invalid_argument);EXPECT_FALSE(std::filesystem::exists(path("underflow.vdb")));
  data.push_back({{0,0,0},-1e-300,1e-300,{}});write("tiny-cancel.vdb",create(data));const auto cancelled=run("value","tiny-cancel.vdb","tiny-zero.vdb");EXPECT_EQ(cancelled.minimum[0],0);EXPECT_EQ(cancelled.field.activeVoxelCount,1);
  o.mode=vdb::PointTransferMode::WeightedMean;o.storage=vdb::PointTransferStorage::Float;
  EXPECT_THROW(run("value","tiny.vdb","float-underflow.vdb"),std::invalid_argument);EXPECT_FALSE(std::filesystem::exists(path("float-underflow.vdb")));
}
TEST_F(OpenVdbPointTransfer, DensityUsesActualReflectedWorldMapAndStoredVectorsReceiveNoFrameTransform) {
  auto data=std::vector<Input>{{{.25,.25,.25},2,3,{2,-4,6}}};auto grid=create(data);
  openvdb::math::Mat4d source(.5,.25,0.,0., 0.,-.75,0.,0., 0.,0.,1.5,0., 10.,-4.,7.,1.);grid->setTransform(openvdb::math::Transform::createLinearTransform(source));write("mapped.vdb",grid);
  const V world=grid->transform().indexToWorld(data[0].p);o.targetIndexToWorld={0,-.5,0,world[0], .5,0,0,world[1], 0,0,-.5,world[2], 0,0,0,1};o.weightAttribute="mass";o.companion=vdb::PointTransferCompanion::WorldDensity;
  const auto result=run("vector","mapped.vdb");EXPECT_DOUBLE_EQ(result.voxelVolume,.125);EXPECT_DOUBLE_EQ(result.totalMass,3);EXPECT_DOUBLE_EQ(result.depositedMass,3);
  auto field=read<openvdb::Vec3DGrid>("out.vdb");auto density=read<openvdb::DoubleGrid>("out.vdb","field_density");EXPECT_EQ(field->tree().getValue({0,0,0}),V(2,-4,6));EXPECT_DOUBLE_EQ(density->tree().getValue({0,0,0}),24);
  EXPECT_EQ(field->transform().indexToWorld(V(0)),world);EXPECT_EQ(field->transform().baseMap()->applyJacobian({0,0,1}),V(0,0,-.5));
}
TEST_F(OpenVdbPointTransfer, FloatOptInReportsRoundingAndRejectsOverflow) {
  auto data=std::vector<Input>{{{0,0,0},1./3.,1,{}}};write("third.vdb",create(data));o.storage=vdb::PointTransferStorage::Float;
  const auto result=run("value","third.vdb");EXPECT_EQ(result.roundedFloatComponents,1);EXPECT_GT(result.maxFloatAbsoluteError,0);EXPECT_FLOAT_EQ(read<openvdb::FloatGrid>("out.vdb")->tree().getValue({0,0,0}),float(1./3.));
  data[0].value=1e100;write("overflow.vdb",create(data));EXPECT_THROW(run("value","overflow.vdb","bad.vdb"),std::invalid_argument);EXPECT_FALSE(std::filesystem::exists(path("bad.vdb")));
}
TEST_F(OpenVdbPointTransfer, EmptySelectionAndNativeArchiveProduceEmptyNumericGrids) {
  o.groups.anyOf={"empty"};const auto result=run();EXPECT_EQ(result.selectedCount,0);EXPECT_EQ(result.totalMass,0);EXPECT_FALSE(result.hasValues);EXPECT_EQ(result.field.activeVoxelCount,0);
  auto archivable=create(records);points::dropAttribute(archivable->tree(),"unrelated_dynamic");write("archivable.vdb",archivable);
  const auto compact=vdb::WriteSelectedPoints(path("archivable.vdb"),path("archived.vdb"),o.groups,vdb::PointSelectionMode::KeepSelected,{},p,limits);ASSERT_TRUE(compact.archivedSchema);
  const auto archived=run("vector","archived.vdb","empty-vector.vdb");EXPECT_TRUE(archived.archivedSchema);EXPECT_EQ(archived.componentCount,3);EXPECT_EQ(archived.selectedCount,0);
}
TEST_F(OpenVdbPointTransfer, SourceCustomMetadataPayloadsAndUnicodeExclusivePublicationStayIntact) {
  const auto original=bytes(path("source.vdb"));const auto parent=directory/std::filesystem::path(std::u8string(u8"転送-é"));std::filesystem::create_directory(parent);
  const auto source=parent/std::filesystem::path(std::u8string(u8"入力.vdb")),output=parent/std::filesystem::path(std::u8string(u8"場.vdb"));std::filesystem::copy_file(path("source.vdb"),source);
  const auto result=vdb::WriteTransferredPointAttribute(source,output,"value",o,{},p,limits);EXPECT_EQ(result.inputCount,3);EXPECT_EQ(original,bytes(source));const auto completed=bytes(output);
  EXPECT_THROW((void)vdb::WriteTransferredPointAttribute(source,output,"value",o,{},p,limits),std::invalid_argument);EXPECT_EQ(completed,bytes(output));
  std::filesystem::copy_file(output,path("unicode-readback.vdb"));
  openvdb::io::File file(path("unicode-readback.vdb").string());file.open(false);EXPECT_EQ(file.getMetadata()->metaValue<int64_t>("file:id"),INT64_C(9007199254740993));auto grid=file.readGrid("field");EXPECT_EQ(grid->metaValue<std::string>("artist"),std::string("a\0b",3));file.close();
}
TEST_F(OpenVdbPointTransfer, ReservedMetadataRegeneratesTypedStructureAndDropsStaleSolverSemantics) {
  auto grid=create(records);
  for(const auto* key:{openvdb::GridBase::META_GRID_CLASS,openvdb::GridBase::META_VECTOR_TYPE,
      openvdb::GridBase::META_IS_LOCAL_SPACE,openvdb::GridBase::META_SAVE_HALF_FLOAT}) {
    grid->removeMeta(key);grid->insertMeta(key,openvdb::Int32Metadata(73));
  }
  const std::array<const char*,10> stale{"container:mac_layout","container:velocity_units",
    "container:pressure_units","container:pressure_gauge","container:projection_dt",
    "container:potential_flow_units","container:potential_flow_boundary",
    "container:surface_field_units","container:surface_field_validity","container:surface_field_location"};
  for(const auto* key:stale)grid->insertMeta(key,openvdb::StringMetadata("source-only semantics"));
  write("foreign-meta.vdb",grid);const auto original=bytes(path("foreign-meta.vdb"));
  EXPECT_NO_THROW((void)run("vector","foreign-meta.vdb"));
  openvdb::io::File file(path("out.vdb").string());file.open(false);
  EXPECT_EQ(file.getMetadata()->metaValue<int64_t>("file:id"),INT64_C(9007199254740993));
  for(const auto* name:{"field","field_weight"}) {
    const auto result=file.readGrid(name);EXPECT_EQ(result->getName(),name);
    EXPECT_EQ(result->getGridClass(),openvdb::GRID_UNKNOWN);
    EXPECT_EQ(result->getVectorType(),openvdb::VEC_INVARIANT);
    EXPECT_TRUE(result->isInWorldSpace());EXPECT_FALSE(result->saveFloatAsHalf());
    EXPECT_TRUE(result->getMetadata<openvdb::StringMetadata>(openvdb::GridBase::META_GRID_CLASS));
    EXPECT_TRUE(result->getMetadata<openvdb::StringMetadata>(openvdb::GridBase::META_VECTOR_TYPE));
    EXPECT_TRUE(result->getMetadata<openvdb::BoolMetadata>(openvdb::GridBase::META_IS_LOCAL_SPACE));
    EXPECT_TRUE(result->getMetadata<openvdb::BoolMetadata>(openvdb::GridBase::META_SAVE_HALF_FLOAT));
    for(const auto* key:stale)EXPECT_FALSE((*result)[key]);
    EXPECT_EQ(result->metaValue<std::string>("artist"),std::string("a\0b",3));
  }
  file.close();EXPECT_EQ(original,bytes(path("foreign-meta.vdb")));
}
TEST_F(OpenVdbPointTransfer, MeanSupportsFullNativeDoubleRangeAndPositiveMassUnderflowNeverDisappears) {
  std::vector<Input> data{{{0,0,0},std::numeric_limits<double>::max(),2,{}}};write("maximum.vdb",create(data));o.weightAttribute="mass";
  EXPECT_NO_THROW((void)run("value","maximum.vdb"));EXPECT_DOUBLE_EQ(read<openvdb::DoubleGrid>("out.vdb")->tree().getValue({0,0,0}),std::numeric_limits<double>::max());
  o.mode=vdb::PointTransferMode::WeightedSum;EXPECT_THROW(run("value","maximum.vdb","overflow.vdb"),std::invalid_argument);
  write("total-mass-overflow.vdb",create({{{0,0,0},1,std::numeric_limits<double>::max(),{}},{{8,0,0},1,std::numeric_limits<double>::max(),{}}}));
  try{(void)run("value","total-mass-overflow.vdb","total-mass-output.vdb");FAIL();}
  catch(const std::invalid_argument& e){EXPECT_NE(std::string(e.what()).find("total selected mass"),std::string::npos);}
  EXPECT_FALSE(std::filesystem::exists(path("total-mass-output.vdb")));
  data[0]={{.5,.5,.5},2,std::numeric_limits<double>::denorm_min(),{}};write("subweight.vdb",create(data));o.mode=vdb::PointTransferMode::WeightedMean;
  EXPECT_THROW(run("value","subweight.vdb","subweight-output.vdb"),std::invalid_argument);EXPECT_FALSE(std::filesystem::exists(path("subweight-output.vdb")));
  data[0].p={0,0,0};write("subdensity.vdb",create(data));o.companion=vdb::PointTransferCompanion::WorldDensity;o.targetIndexToWorld={1e6,0,0,0,0,1e6,0,0,0,0,1e6,0,0,0,0,1};
  EXPECT_THROW(run("value","subdensity.vdb","subdensity-output.vdb"),std::invalid_argument);EXPECT_FALSE(std::filesystem::exists(path("subdensity-output.vdb")));
}
TEST_F(OpenVdbPointTransfer, NumericLayoutGroupCoordinateAndAllSharedCapsFailBeforePublication) {
  for(int which=0;which<8;++which){auto saved=o;auto pp=p;auto ll=limits;
    if(which==0)o.maxWork=1;if(which==1)o.maxMetadataBytes=1;if(which==2)p.maxInputBytes=1024;if(which==3)p.maxVoxelCount=512;
    if(which==4)limits.maxPoints=1;if(which==5)limits.maxAttributes=2;if(which==6)limits.maxGroups=1;if(which==7)o.companionGridName="field";
    EXPECT_THROW(run(),std::invalid_argument)<<which;EXPECT_FALSE(std::filesystem::exists(path("out.vdb")));o=saved;p=pp;limits=ll;}
  for(const auto* attribute:{"P","__group","missing","unrelated_dynamic"}){EXPECT_THROW(run(attribute),std::exception);EXPECT_FALSE(std::filesystem::exists(path("out.vdb")));}
  o.weightAttribute="vector";
  EXPECT_THROW(run(),std::invalid_argument);
  o.weightAttribute="mass";o.constantMass=2;
  EXPECT_THROW(run(),std::invalid_argument);
  o={};o.targetIndexToWorld={1,0,0,1e15,0,1,0,0,0,0,1,0,0,0,0,1};EXPECT_THROW(run(),std::invalid_argument);
  EXPECT_FALSE(std::filesystem::exists(path("out.vdb")));
}
TEST_F(OpenVdbPointTransfer, ExactComplementsRetainBelowEpsilonFractionalMassAndReportSubUlpResiduals) {
  const double tiny=double(std::numeric_limits<float>::denorm_min());
  write("fraction.vdb",create({{{tiny,tiny,tiny},9,1,{}}}));
  o.targetIndexToWorld={1e6,0,0,0,0,1e6,0,0,0,0,1e6,0,0,0,0,1};
  const auto result=run("value","fraction.vdb");EXPECT_EQ(result.contributionCount,8);EXPECT_DOUBLE_EQ(result.totalMass,1);EXPECT_DOUBLE_EQ(result.depositedMass,1);
  auto field=read<openvdb::DoubleGrid>("out.vdb");auto weight=read<openvdb::DoubleGrid>("out.vdb","field_weight");
  const double f=tiny*1e-6;EXPECT_DOUBLE_EQ(weight->tree().getValue({1,0,0}),f);
  EXPECT_NEAR(weight->tree().getValue({1,1,1}),f*f*f,f*f*f*1e-14);
  EXPECT_GT(result.massErrorUpperBound,0);EXPECT_NEAR(result.massErrorUpperBound,3*f,1e-60);
  for(auto i=field->cbeginValueOn();i;++i)EXPECT_DOUBLE_EQ(*i,9);
  write("negative-fraction.vdb",create({{{-tiny,-tiny,-tiny},9,1,{}}}));
  const auto negative=run("value","negative-fraction.vdb","negative-fraction-output.vdb");
  EXPECT_EQ(negative.contributionCount,8);EXPECT_DOUBLE_EQ(negative.totalMass,1);EXPECT_DOUBLE_EQ(negative.depositedMass,1);
  auto negativeField=read<openvdb::DoubleGrid>("negative-fraction-output.vdb");
  auto negativeWeight=read<openvdb::DoubleGrid>("negative-fraction-output.vdb","field_weight");
  for(auto i=weight->cbeginValueOn();i;++i)EXPECT_EQ(negativeWeight->tree().getValue(-i.getCoord()),*i);
  for(auto i=negativeField->cbeginValueOn();i;++i)EXPECT_DOUBLE_EQ(*i,9);
}
TEST_F(OpenVdbPointTransfer, TinyTranslatedReflectedTargetUsesExactNativeMapAndReportedDeterminant) {
  write("origin.vdb",create({{{0,0,0},2,1,{}}}));
  o.targetIndexToWorld={-1e-6,0,0,2e-6,0,1e-6,0,0,0,0,1e-6,0,0,0,0,1};
  o.companion=vdb::PointTransferCompanion::WorldDensity;
  const auto result=run("value","origin.vdb");EXPECT_NEAR(result.voxelVolume,1e-18,1e-32);EXPECT_NEAR(result.depositedMass,1,1e-14);
  auto field=read<openvdb::DoubleGrid>("out.vdb");EXPECT_EQ(field->transform().indexToWorld(V(0)),V(2e-6,0,0));
  EXPECT_EQ(field->transform().baseMap()->applyJacobian({1,0,0}),V(-1e-6,0,0));
  EXPECT_DOUBLE_EQ(field->tree().getValue({2,0,0}),2);
}
TEST_F(OpenVdbPointTransfer, MandatoryDelayedLoadingRejectsBeforePayloadMaterialization) {
  struct Restore {
    std::string value;bool present=false;
    Restore(){const auto* x=std::getenv("OPENVDB_DISABLE_DELAYED_LOAD");present=x;if(x)value=x;
#ifdef _WIN32
      _putenv_s("OPENVDB_DISABLE_DELAYED_LOAD","1");
#else
      setenv("OPENVDB_DISABLE_DELAYED_LOAD","1",1);
#endif
    }
    ~Restore(){
#ifdef _WIN32
      _putenv_s("OPENVDB_DISABLE_DELAYED_LOAD",present?value.c_str():"");
#else
      if(present)setenv("OPENVDB_DISABLE_DELAYED_LOAD",value.c_str(),1);else unsetenv("OPENVDB_DISABLE_DELAYED_LOAD");
#endif
    }
  }restore;
  try{(void)run();FAIL();}catch(const std::invalid_argument& e){EXPECT_NE(std::string(e.what()).find("delayed loading"),std::string::npos);}
  EXPECT_FALSE(std::filesystem::exists(path("out.vdb")));
}
#endif
} // namespace
