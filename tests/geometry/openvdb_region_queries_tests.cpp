#include "Container/geometry/OpenVdbRegionQueries.h"
#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <gtest/gtest.h>
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
#include "../../src/geometry/OpenVdbTransformUtils.h"
#include <openvdb/points/PointDataGrid.h>
#include <sstream>
#endif
namespace {
namespace vdb=container::geometry::vdb;
TEST(OpenVdbRegionQueriesAvailability, DisabledBeforeOptionsAndPaths){
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  GTEST_SKIP();
#else
  vdb::ActiveRegionOptions options;options.maxInputBytes=0;const std::array<vdb::IndexRegion,1> invalid{{{{2,0,0},{1,0,0}}}};
  try{(void)vdb::QueryActiveRegions("missing",invalid,options);FAIL();}catch(const std::runtime_error& e){EXPECT_NE(std::string(e.what()).find("ENABLE_OPENVDB=ON"),std::string::npos);}
#endif
}
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
template<class G>class ExactRegionFixture:public G {
public:explicit ExactRegionFixture(const G& g):G(openvdb::ConstPtrCast<typename G::TreeType>(g.constTreePtr())){this->insertMeta(g);this->setTransform(vdb::detail::CopyTransformExact(g.transform()));}
  openvdb::GridBase::ConstPtr copyGrid()const override{return this->copyReplacingMetadataAndTransform(*this,vdb::detail::CopyTransformExact(this->transform()));}
  openvdb::GridBase::Ptr copyGrid()override{return openvdb::ConstPtrCast<openvdb::GridBase>(static_cast<const ExactRegionFixture&>(*this).copyGrid());}
};
class OpenVdbRegionQueries:public ::testing::Test {
protected:
  std::filesystem::path directory;
  void SetUp()override{openvdb::initialize();static std::atomic_uint64_t serial{};directory=std::filesystem::temp_directory_path()/("container-region-query-tests-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+"-"+std::to_string(++serial));ASSERT_TRUE(std::filesystem::create_directory(directory));}
  void TearDown()override{std::error_code e;if(std::filesystem::is_symlink(directory,e)||e)return;const auto target=std::filesystem::weakly_canonical(directory,e);if(e)return;const auto parent=std::filesystem::weakly_canonical(std::filesystem::temp_directory_path(),e);if(!e&&target.parent_path()==parent&&target.filename()==directory.filename()&&target.filename().string().starts_with("container-region-query-tests-"))std::filesystem::remove_all(target,e);}
  auto path(const char* name)const{return directory/name;}
  void write(const char* name,const openvdb::GridPtrVec& grids,const openvdb::MetaMap& metadata={}){openvdb::io::File file(path(name).string());file.setCompression(openvdb::io::COMPRESS_ZIP);file.write(grids,metadata);file.close();}
  static std::string contents(const std::filesystem::path& p){std::ifstream in(p,std::ios::binary);return {std::istreambuf_iterator<char>(in),{}};}
  auto run(const char* source,std::span<const vdb::IndexRegion> regions,const vdb::ActiveRegionOptions& options={},const std::string& selector={}){return vdb::QueryActiveRegions(path(source),regions,options,selector);}
  template<class G>void one(const char* name,typename G::ValueType native,vdb::RegionNativeValue expected,vdb::RegionValueType type){auto grid=G::create();grid->setName("field");grid->tree().touchLeaf(openvdb::Coord(-1,2,3))->setValueOn(openvdb::Coord(-1,2,3),native);write(name,{grid});const auto original=contents(path(name));const std::array<vdb::IndexRegion,1> roi{{{{-2,1,2},{0,3,4}}}};const auto result=run(name,roi);ASSERT_EQ(result.queries.size(),1u);ASSERT_EQ(result.queries[0].records.size(),1u);const auto& record=result.queries[0].records[0];EXPECT_EQ(result.nativeType,type);EXPECT_EQ(record.value,expected);EXPECT_FALSE(record.tile);EXPECT_EQ(record.sourceLevel,0u);EXPECT_EQ(record.voxelCount,1u);EXPECT_EQ(record.clipped.minimum,(std::array<int32_t,3>{-1,2,3}));EXPECT_EQ(record.clipped.maximum,record.clipped.minimum);EXPECT_EQ(result.sourceActiveVoxels,1u);EXPECT_TRUE(result.queries[0].countsComplete);EXPECT_FALSE(result.queries[0].recordsTruncated);EXPECT_EQ(contents(path(name)),original);}
};
TEST_F(OpenVdbRegionQueries, AllNineNativeTypesRetainExactValuesAndRawVectorComponents){
  one<openvdb::BoolGrid>("bool.vdb",false,false,vdb::RegionValueType::Boolean);
  one<openvdb::MaskGrid>("mask.vdb",true,true,vdb::RegionValueType::Mask);
  one<openvdb::Int32Grid>("int32.vdb",INT32_MIN,INT32_MIN,vdb::RegionValueType::Int32);
  one<openvdb::Int64Grid>("int64.vdb",INT64_C(9007199254740993),INT64_C(9007199254740993),vdb::RegionValueType::Int64);
  one<openvdb::FloatGrid>("float.vdb",std::bit_cast<float>(uint32_t(0x80000000)),vdb::RegionFloat32Bits{0x80000000},vdb::RegionValueType::Float);
  one<openvdb::DoubleGrid>("double.vdb",std::bit_cast<double>(uint64_t(0x8000000000000000)),vdb::RegionFloat64Bits{0x8000000000000000},vdb::RegionValueType::Double);
  one<openvdb::Vec3IGrid>("vec3i.vdb",openvdb::Vec3i(INT32_MIN,0,INT32_MAX),std::array<int32_t,3>{INT32_MIN,0,INT32_MAX},vdb::RegionValueType::Vec3I);
  one<openvdb::Vec3SGrid>("vec3s.vdb",openvdb::Vec3s(-0.f,std::bit_cast<float>(uint32_t(0x7f812345)),-2.f),std::array<vdb::RegionFloat32Bits,3>{{{0x80000000},{0x7f812345},{0xc0000000}}},vdb::RegionValueType::Vec3S);
  one<openvdb::Vec3DGrid>("vec3d.vdb",openvdb::Vec3d(-0.,std::bit_cast<double>(uint64_t(0x7ff0000000001234)),-2.),std::array<vdb::RegionFloat64Bits,3>{{{0x8000000000000000},{0x7ff0000000001234},{0xc000000000000000}}},vdb::RegionValueType::Vec3D);
}
TEST_F(OpenVdbRegionQueries, NonfinitePayloadBitsAndInactiveContinuationsAreIndependentOfActivity){
  constexpr uint32_t fbits=0x7f812345;constexpr uint64_t dbits=0xfff0000000001234;
  auto f=openvdb::FloatGrid::create(std::bit_cast<float>(uint32_t(0xffc54321)));f->setName("float");auto* leaf=f->tree().touchLeaf(openvdb::Coord(0));leaf->setValueOn(openvdb::Coord(0));leaf->setValueOff(openvdb::Coord(1,0,0),std::bit_cast<float>(uint32_t(0x7f800000)));
  // Scalar FP argument evaluation can quiet a signaling NaN before the setter.
  // Establish the native storage bits directly, then verify both prewrite and
  // independent SDK readback before exercising the query API.
  std::memcpy(leaf->buffer().data(),&fbits,sizeof(fbits));
  ASSERT_EQ(std::bit_cast<uint32_t>(leaf->getValue(openvdb::Coord(0))),fbits)<<"native fixture before serialization";
  write("nan-f.vdb",{f});
  {openvdb::io::File native(path("nan-f.vdb").string());native.open(false);const auto loaded=openvdb::gridPtrCast<openvdb::FloatGrid>(native.readGrid("float"));ASSERT_EQ(std::bit_cast<uint32_t>(loaded->tree().getValue(openvdb::Coord(0))),fbits)<<"independent SDK readback";native.close();}
  const std::array<vdb::IndexRegion,1> roi{{{{-1,-1,-1},{2,1,1}}}};const auto rf=run("nan-f.vdb",roi);ASSERT_EQ(rf.queries[0].records.size(),1u);EXPECT_EQ(std::get<vdb::RegionFloat32Bits>(rf.queries[0].records[0].value).bits,fbits);EXPECT_EQ(std::get<vdb::RegionFloat32Bits>(rf.background).bits,0xffc54321u);
  auto d=openvdb::DoubleGrid::create(0.);auto* dl=d->tree().touchLeaf(openvdb::Coord(0));dl->setValueOn(openvdb::Coord(0));std::memcpy(dl->buffer().data(),&dbits,sizeof(dbits));ASSERT_EQ(std::bit_cast<uint64_t>(dl->getValue(openvdb::Coord(0))),dbits);write("nan-d.vdb",{d});const auto rd=run("nan-d.vdb",roi);EXPECT_EQ(std::get<vdb::RegionFloat64Bits>(rd.queries[0].records[0].value).bits,dbits);
  auto b=openvdb::BoolGrid::create(true);b->tree().touchLeaf(openvdb::Coord(0))->setValueOn(openvdb::Coord(0),false);b->tree().touchLeaf(openvdb::Coord(0))->setValueOff(openvdb::Coord(1,0,0),true);write("activity.vdb",{b});const auto rb=run("activity.vdb",roi);EXPECT_EQ(rb.sourceActiveVoxels,1u);EXPECT_FALSE(std::get<bool>(rb.queries[0].records[0].value));EXPECT_TRUE(std::get<bool>(rb.background));
}
TEST_F(OpenVdbRegionQueries, HugeRootTilesAreClippedWithExactUInt64MultiplicityWithoutExpansion){
  auto grid=openvdb::Int64Grid::create(0);grid->setName("tiles");grid->tree().addTile(3,openvdb::Coord(0),INT64_C(9007199254740993),true);grid->tree().addTile(3,openvdb::Coord(4096,0,0),INT64_MAX,true);write("tiles.vdb",{grid});
  const std::array<vdb::IndexRegion,2> roi{{{{4095,2,3},{4097,3,4}},{{0,0,0},{4095,4095,4095}}}};vdb::ActiveRegionOptions o;o.maxStoredLeafVoxels=1;o.maxNodes=3;const auto r=run("tiles.vdb",roi,o);EXPECT_EQ(r.sourceActiveVoxels,uint64_t(1)<<37);EXPECT_EQ(r.sourceActiveTileVoxels,r.sourceActiveVoxels);EXPECT_EQ(r.sourceActiveTiles,2u);EXPECT_EQ(r.sourceRootEntries,2u);EXPECT_EQ(r.sourceLeafNodes,0u);EXPECT_EQ(r.storedLeafVoxels,0u);EXPECT_EQ(r.nodeAndRootEntryCount,3u);
  ASSERT_EQ(r.queries[0].records.size(),2u);EXPECT_EQ(r.queries[0].matchedVoxelCount,12u);std::vector<uint64_t> counts;for(const auto& x:r.queries[0].records){EXPECT_TRUE(x.tile);EXPECT_EQ(x.sourceLevel,3u);counts.push_back(x.voxelCount);}std::sort(counts.begin(),counts.end());EXPECT_EQ(counts,(std::vector<uint64_t>{4,8}));EXPECT_EQ(r.queries[1].matchedVoxelCount,uint64_t(1)<<36);EXPECT_EQ(r.queries[1].records[0].voxelCount,uint64_t(1)<<36);
}
TEST_F(OpenVdbRegionQueries, Int32EndpointRoisNeverUseOverflowingMidpointsOrDimensions){
  auto grid=openvdb::Int32Grid::create(0);grid->tree().addTile(3,openvdb::Coord(INT32_MIN),7,true);grid->tree().addTile(3,openvdb::Coord(INT32_MAX),9,true);write("endpoints.vdb",{grid});const std::array<vdb::IndexRegion,3> roi{{{{INT32_MIN,INT32_MIN,INT32_MIN},{INT32_MIN,INT32_MIN,INT32_MIN}},{{INT32_MAX,INT32_MAX,INT32_MAX},{INT32_MAX,INT32_MAX,INT32_MAX}},{{INT32_MIN,INT32_MIN,INT32_MIN},{INT32_MAX,INT32_MAX,INT32_MAX}}}};const auto result=run("endpoints.vdb",roi);EXPECT_EQ(result.queries[0].matchedVoxelCount,1u);EXPECT_EQ(result.queries[1].matchedVoxelCount,1u);EXPECT_EQ(result.queries[2].matchedVoxelCount,uint64_t(1)<<37);EXPECT_EQ(std::get<int32_t>(result.queries[0].records[0].value),7);EXPECT_EQ(std::get<int32_t>(result.queries[1].records[0].value),9);
}
TEST_F(OpenVdbRegionQueries, TruncationKeepsCompleteCountsAndQueryOrderGetsAggregateQuota){
  auto g=openvdb::Int32Grid::create(0);g->tree().setValueOn(openvdb::Coord(-1,0,0),1);g->tree().setValueOn(openvdb::Coord(1,0,0),2);g->tree().setValueOn(openvdb::Coord(2,0,0),3);write("prefix.vdb",{g});const std::array<vdb::IndexRegion,3> roi{{{{50,50,50},{51,51,51}},{{-2,0,0},{3,0,0}},{{-2,0,0},{3,0,0}}}};const auto full=run("prefix.vdb",roi);vdb::ActiveRegionOptions o;o.maxTotalRecords=1;o.maxRecordsPerQuery=2;const auto r=run("prefix.vdb",roi,o);EXPECT_TRUE(r.countsComplete);EXPECT_TRUE(r.queries[0].records.empty());ASSERT_EQ(r.queries[1].records.size(),1u);EXPECT_TRUE(r.queries[2].records.empty());for(size_t i=1;i<3;++i){EXPECT_TRUE(r.queries[i].countsComplete);EXPECT_TRUE(r.queries[i].recordsTruncated);EXPECT_EQ(r.queries[i].matchedVoxelCount,3u);EXPECT_EQ(r.queries[i].matchedRecordCount,3u);}EXPECT_EQ(r.queries[1].omittedRecordCount,2u);EXPECT_EQ(r.queries[2].omittedRecordCount,3u);EXPECT_EQ(r.queries[1].records[0].source,full.queries[1].records[0].source);EXPECT_EQ(r.queries[1].records[0].value,full.queries[1].records[0].value);
  o.maxTotalRecords=0;o.maxRecordsPerQuery=0;const auto counts=run("prefix.vdb",roi,o);EXPECT_EQ(counts.queries[1].matchedVoxelCount,3u);EXPECT_TRUE(counts.queries[1].records.empty());EXPECT_EQ(counts.queries[1].omittedVoxelCount,3u);
}
TEST_F(OpenVdbRegionQueries, EmptyBatchStillCensusesAndEmptyTreeHasCompleteZeroCounts){
  auto grid=openvdb::DoubleGrid::create(-0.);grid->tree().addTile(3,openvdb::Coord(0),0.,true);write("census.vdb",{grid});const auto census=run("census.vdb",{});EXPECT_TRUE(census.queries.empty());EXPECT_EQ(census.sourceActiveVoxels,uint64_t(1)<<36);EXPECT_EQ(census.sourceActiveRecords,1u);vdb::ActiveRegionOptions o;o.maxSourceActiveVoxels=(uint64_t(1)<<36)-1;
  EXPECT_THROW((void)run("census.vdb",{},o),std::invalid_argument);o={};o.maxWork=census.workUpperBound-1;
  EXPECT_THROW((void)run("census.vdb",{},o),std::invalid_argument);
  auto empty=openvdb::DoubleGrid::create(-0.);write("empty.vdb",{empty});const std::array<vdb::IndexRegion,1> roi{{{{INT32_MIN,INT32_MIN,INT32_MIN},{INT32_MAX,INT32_MAX,INT32_MAX}}}};const auto e=run("empty.vdb",roi);EXPECT_EQ(e.sourceActiveVoxels,0u);EXPECT_TRUE(e.queries[0].countsComplete);EXPECT_FALSE(e.queries[0].recordsTruncated);EXPECT_EQ(std::get<vdb::RegionFloat64Bits>(e.background).bits,0x8000000000000000ull);
}
TEST_F(OpenVdbRegionQueries, BinaryTypedMetadataAndCanonicalKeysRemainReadOnlyAndExact){
  auto grid=openvdb::Int64Grid::create(0);grid->tree().setValueOn(openvdb::Coord(0),INT64_MIN);grid->insertMeta("artist",openvdb::StringMetadata(std::string("raw\0\xff",5)));grid->insertMeta("container:boundary_projection_state_id",openvdb::Int32Metadata(7));openvdb::MetaMap fileMeta;fileMeta.insertMeta("scene",openvdb::Int64Metadata(INT64_C(9007199254740993)));write("meta.vdb",{grid},fileMeta);const auto original=contents(path("meta.vdb"));const auto r=run("meta.vdb",{});
  const auto find=[](const auto& rows,const char* name)->const vdb::RegionQueryMetadata*{for(const auto& row:rows)if(row.name==name)return &row;return nullptr;};const auto* artist=find(r.gridMetadata,"artist");ASSERT_NE(artist,nullptr);ASSERT_EQ(artist->serializedValue.size(),9u);EXPECT_EQ(std::string(reinterpret_cast<const char*>(artist->serializedValue.data()+4),5),std::string("raw\0\xff",5));const auto* key=find(r.gridMetadata,"container:boundary_projection_state_id");ASSERT_NE(key,nullptr);EXPECT_EQ(key->type,"int32");const auto* scene=find(r.fileMetadata,"scene");ASSERT_NE(scene,nullptr);ASSERT_EQ(scene->serializedValue.size(),12u);int64_t exact{};std::memcpy(&exact,scene->serializedValue.data()+4,8);EXPECT_EQ(exact,INT64_C(9007199254740993));EXPECT_EQ(contents(path("meta.vdb")),original);EXPECT_EQ(std::get<int64_t>(r.background),0);
}
TEST_F(OpenVdbRegionQueries, TinyRemoteAffineAndNonlinearMapRecordsDoNotAlterIndexQueries){
  auto grid=openvdb::DoubleGrid::create(0.);grid->tree().setValueOn(openvdb::Coord(0),2.);std::stringstream encoded(std::ios::binary|std::ios::in|std::ios::out);openvdb::writeString(encoded,openvdb::math::UniformScaleTranslateMap::mapType());openvdb::Vec3d(1e9,-1e9,3).write(encoded);for(const double n:{1e-6,1e-6,1e6,1e12,5e5})openvdb::Vec3d(n).write(encoded);encoded.seekg(0);openvdb::io::setCurrentVersion(encoded);auto transform=std::make_shared<openvdb::math::Transform>();transform->read(encoded);grid->setTransform(transform);write("tiny-map.vdb",{std::make_shared<ExactRegionFixture<openvdb::DoubleGrid>>(*grid)});const auto r=run("tiny-map.vdb",{});ASSERT_TRUE(r.indexToWorld);EXPECT_EQ((*r.indexToWorld)[0],1e-6);EXPECT_EQ((*r.indexToWorld)[3],1e9);EXPECT_EQ((*r.indexToWorld)[5],1e-6);EXPECT_EQ((*r.indexToWorld)[7],-1e9);EXPECT_EQ(r.transformType,openvdb::math::UniformScaleTranslateMap::mapType());EXPECT_FALSE(r.serializedTransform.empty());EXPECT_EQ(r.sourceActiveVoxels,1u);
  auto nonlinear=std::make_shared<openvdb::math::Transform>(std::make_shared<openvdb::math::NonlinearFrustumMap>());grid->setTransform(nonlinear);write("nonlinear.vdb",{grid});const std::array<vdb::IndexRegion,1> roi{{{{0,0,0},{0,0,0}}}};const auto n=run("nonlinear.vdb",roi);EXPECT_FALSE(n.affine);EXPECT_FALSE(n.indexToWorld);EXPECT_FALSE(n.serializedTransform.empty());EXPECT_EQ(n.queries[0].matchedVoxelCount,1u);
}
TEST_F(OpenVdbRegionQueries, StrictSelectorsUnsupportedPointsAndIndependentRawVectorTags){
  auto a=openvdb::FloatGrid::create(0.f);auto b=openvdb::DoubleGrid::create(0.);a->setName("same");b->setName("same");a->tree().setValueOn(openvdb::Coord(0),1.f);b->tree().setValueOn(openvdb::Coord(0),2.);write("duplicate.vdb",{a,b});
  EXPECT_THROW((void)run("duplicate.vdb",{}),std::invalid_argument);
  EXPECT_THROW((void)run("duplicate.vdb",{}, {},"same"),std::invalid_argument);openvdb::io::File names(path("duplicate.vdb").string());names.open(false);std::vector<std::string> descriptors;for(auto it=names.beginName();it!=names.endName();++it)descriptors.push_back(it.gridName());names.close();ASSERT_EQ(descriptors.size(),2u);const auto chosen=run("duplicate.vdb",{}, {},"descriptor:"+descriptors[0]);EXPECT_EQ(chosen.descriptorName,descriptors[0]);
  EXPECT_THROW((void)run("duplicate.vdb",{}, {},"missing"),std::invalid_argument);
  auto points=openvdb::points::PointDataGrid::create();points->setName("points");write("points.vdb",{points});
  EXPECT_THROW((void)run("points.vdb",{}, {},"points"),std::invalid_argument);
  auto vector=openvdb::Vec3DGrid::create(openvdb::Vec3d(0));vector->setName("positions");vector->setVectorType(openvdb::VEC_CONTRAVARIANT_ABSOLUTE);vector->setIsInWorldSpace(false);vector->tree().setValueOn(openvdb::Coord(0),openvdb::Vec3d(2,3,4));vector->transform().postTranslate(openvdb::Vec3d(10,20,30));write("raw-vector.vdb",{vector});const std::array<vdb::IndexRegion,1> roi{{{{0,0,0},{0,0,0}}}};const auto v=run("raw-vector.vdb",roi);EXPECT_FALSE(v.inWorldSpace);EXPECT_EQ((std::get<std::array<vdb::RegionFloat64Bits,3>>(v.queries[0].records[0].value)[0].bits),std::bit_cast<uint64_t>(2.));
}
TEST_F(OpenVdbRegionQueries, UnicodeInputsAndSharedCapsPreserveSourceBytes){
  auto grid=openvdb::Int32Grid::create(0);grid->setName("field");grid->tree().setValueOn(openvdb::Coord(0),1);write("plain.vdb",{grid});const auto unicode=directory/std::filesystem::path(L"\u03bb-\u67e5\u8be2.vdb");std::filesystem::copy_file(path("plain.vdb"),unicode);const auto original=contents(unicode);const std::array<vdb::IndexRegion,1> roi{{{{0,0,0},{0,0,0}}}};const auto baseline=vdb::QueryActiveRegions(unicode,roi);EXPECT_EQ(baseline.queries[0].matchedVoxelCount,1u);
  std::vector<vdb::ActiveRegionOptions> caps(8);caps[0].maxInputBytes=1024;caps[1].maxStoredLeafVoxels=511;caps[2].maxNodes=baseline.nodeAndRootEntryCount-1;caps[3].maxWork=baseline.workUpperBound-1;caps[4].maxMetadataBytes=1;caps[5].maxQueries=0;caps[6].maxGrids=0;caps[7].maxSourceActiveVoxels=0;for(const auto& cap:caps)
  EXPECT_THROW((void)vdb::QueryActiveRegions(unicode,roi,cap),std::invalid_argument);const std::array<vdb::IndexRegion,1> invalid{{{{1,0,0},{0,0,0}}}};
  EXPECT_THROW((void)vdb::QueryActiveRegions(unicode,invalid),std::invalid_argument);EXPECT_EQ(contents(unicode),original);
}
struct DisabledPaging {
  std::optional<std::string> previous;
  DisabledPaging(){if(const auto* p=std::getenv("OPENVDB_DISABLE_DELAYED_LOAD"))previous=p;
#if defined(_WIN32)
    _putenv_s("OPENVDB_DISABLE_DELAYED_LOAD","1");
#else
    setenv("OPENVDB_DISABLE_DELAYED_LOAD","1",1);
#endif
  }
  ~DisabledPaging(){
#if defined(_WIN32)
    _putenv_s("OPENVDB_DISABLE_DELAYED_LOAD",previous?previous->c_str():"");
#else
    if(previous)setenv("OPENVDB_DISABLE_DELAYED_LOAD",previous->c_str(),1);else unsetenv("OPENVDB_DISABLE_DELAYED_LOAD");
#endif
  }
};
TEST_F(OpenVdbRegionQueries, DisabledDelayedLoadingRejectsBeforeTypedPayloadPaging){auto grid=openvdb::FloatGrid::create(0.f);write("source.vdb",{grid});DisabledPaging disabled;try{(void)run("source.vdb",{});FAIL();}catch(const std::invalid_argument& e){EXPECT_NE(std::string(e.what()).find("delayed loading"),std::string::npos);}}
#endif
} // namespace
