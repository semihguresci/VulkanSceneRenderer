#include "Container/geometry/OpenVdbValueComponents.h"
#include <atomic>
#include <bit>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <gtest/gtest.h>
#include <memory>
#include <type_traits>
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
#include "../../src/geometry/OpenVdbTransformUtils.h"
#include <openvdb/io/File.h>
#include <openvdb/openvdb.h>
#include <set>
#include <sstream>
#endif
namespace {
namespace vdb=container::geometry::vdb;
TEST(OpenVdbValueComponentsAvailability, DisabledBeforeOptionsAndPaths){
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  GTEST_SKIP();
#else
  vdb::ActiveValueComponentsOptions o;o.maxComponents=0;o.maxInputBytes=0;try{(void)vdb::WriteActiveValueComponents("missing","",o);FAIL();}catch(const std::runtime_error& e){EXPECT_NE(std::string(e.what()).find("ENABLE_OPENVDB=ON"),std::string::npos);}
#endif
}
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
template<class Grid>class ExactFixture final:public Grid {
public:explicit ExactFixture(const Grid& g):Grid(openvdb::ConstPtrCast<typename Grid::TreeType>(g.constTreePtr())){this->insertMeta(g);this->setTransform(vdb::detail::CopyTransformExact(g.transform()));this->setSaveFloatAsHalf(false);}
  openvdb::GridBase::ConstPtr copyGrid()const override{return this->copyReplacingMetadataAndTransform(*this,vdb::detail::CopyTransformExact(this->transform()));}
  openvdb::GridBase::Ptr copyGrid()override{return openvdb::ConstPtrCast<openvdb::GridBase>(static_cast<const ExactFixture&>(*this).copyGrid());}
};
class OpenVdbValueComponents:public ::testing::Test {
protected:
  std::filesystem::path directory;std::vector<std::pair<std::filesystem::path,std::string>> originals;
  void SetUp()override{openvdb::initialize();static std::atomic_uint64_t serial{};directory=std::filesystem::temp_directory_path()/("container-value-components-tests-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+"-"+std::to_string(++serial));ASSERT_TRUE(std::filesystem::create_directory(directory));}
  void TearDown()override{for(const auto& [file,before]:originals)EXPECT_EQ(contents(file),before);std::error_code e;if(std::filesystem::is_symlink(directory,e)||e)return;const auto target=std::filesystem::weakly_canonical(directory,e);if(e)return;const auto parent=std::filesystem::weakly_canonical(std::filesystem::temp_directory_path(),e);if(!e&&target.parent_path()==parent&&target.filename()==directory.filename()&&target.filename().string().starts_with("container-value-components-tests-"))std::filesystem::remove_all(target,e);}
  auto path(const char* name)const{return directory/name;}
  static std::string contents(const std::filesystem::path& p){std::ifstream stream(p,std::ios::binary);return {std::istreambuf_iterator<char>(stream),{}};}
  static openvdb::MetaMap fileMetadata(){openvdb::MetaMap m;m.insertMeta("scene",openvdb::Int64Metadata(INT64_C(9007199254740993)));m.insertMeta("binary",openvdb::StringMetadata(std::string("raw\0\xff",5)));return m;}
  template<class Grid>void write(const char* name,const typename Grid::Ptr& g){openvdb::io::File file(path(name).string());file.setCompression(openvdb::io::COMPRESS_ZIP);file.write({std::make_shared<ExactFixture<Grid>>(*g)},fileMetadata());file.close();originals.emplace_back(path(name),contents(path(name)));}
  template<class Grid>static auto read(const std::filesystem::path& p,const std::string& name){openvdb::io::File file(p.string());file.open(false);auto grid=openvdb::gridPtrCast<Grid>(file.readGrid(name));file.close();return grid;}
  static std::vector<std::string> names(const std::filesystem::path& p){openvdb::io::File file(p.string());file.open(false);std::vector<std::string> names;for(auto i=file.beginName();i!=file.endName();++i)names.push_back(i.gridName());file.close();return names;}
  template<class Grid>static void raw(Grid& g,const openvdb::Coord& c,const typename Grid::ValueType& value,bool active=true){auto* leaf=g.tree().touchLeaf(c);if constexpr(std::is_same_v<Grid,openvdb::MaskGrid>){if(active)leaf->setValueOn(c);else leaf->setValueOff(c);}else if constexpr(std::is_same_v<typename Grid::ValueType,bool>){if(active)leaf->setValueOn(c,value);else leaf->setValueOff(c,value);}else{leaf->setValueOn(c);std::memcpy(leaf->buffer().data()+leaf->coordToOffset(c),&value,sizeof(value));if(!active)leaf->setValueOff(c);}}
  template<class Value>static void sameBits(const Value& a,const Value& b){EXPECT_EQ(std::memcmp(&a,&b,sizeof(Value)),0);}
  static void stamp(openvdb::GridBase& grid){grid.setName("source");grid.setGridClass(openvdb::GRID_UNKNOWN);grid.setIsInWorldSpace(false);grid.setVectorType(openvdb::VEC_CONTRAVARIANT_ABSOLUTE);grid.insertMeta("custom",openvdb::Int64Metadata(INT64_C(9007199254740993)));grid.insertMeta("binary",openvdb::StringMetadata(std::string("raw\0\xff",5)));grid.insertMeta("container:boundary_custom",openvdb::StringMetadata("preserve"));for(const char* key:{"container:boundary_projection_schema","container:boundary_face_channels","container:boundary_projection_state_id","container:boundary_clamped_faces","container:boundary_maximum_clamp"})grid.insertMeta(key,openvdb::Int32Metadata(42));}
  static void metadataChecks(const openvdb::GridBase& grid){EXPECT_EQ(grid.metaValue<int64_t>("custom"),INT64_C(9007199254740993));EXPECT_EQ(grid.metaValue<std::string>("binary"),std::string("raw\0\xff",5));EXPECT_EQ(grid.metaValue<std::string>("container:boundary_custom"),"preserve");for(const char* key:{"container:boundary_projection_schema","container:boundary_face_channels","container:boundary_projection_state_id","container:boundary_clamped_faces","container:boundary_maximum_clamp"})EXPECT_FALSE(grid[key]);}
  void checkFileMetadata(const char* name){openvdb::io::File file(path(name).string());file.open(false);const auto m=file.getMetadata();EXPECT_EQ(m->metaValue<int64_t>("scene"),INT64_C(9007199254740993));EXPECT_EQ(m->metaValue<std::string>("binary"),std::string("raw\0\xff",5));file.close();}
  template<class Grid>void typed(const char* input,const char* output,const typename Grid::ValueType& background,const typename Grid::ValueType& activeValue,const typename Grid::ValueType& inactiveValue){
    auto g=Grid::create(background);stamp(*g);const std::array<openvdb::Coord,4> points{openvdb::Coord(-9,0,0),openvdb::Coord(-8,0,0),openvdb::Coord(1,1,0),openvdb::Coord(2,2,0)};for(const auto& c:points)raw(*g,c,activeValue);const openvdb::Coord inactive(1000000000,-3,2);raw(*g,inactive,inactiveValue,false);g->tree().addTile(3,openvdb::Coord(8192),inactiveValue,false);sameBits(g->tree().getValue(points[0]),activeValue);sameBits(g->tree().getValue(inactive),inactiveValue);write<Grid>(input,g);
    const auto source=read<Grid>(path(input),"source");sameBits(source->tree().getValue(points[0]),activeValue);sameBits(source->tree().getValue(inactive),inactiveValue);
    const auto r=vdb::WriteActiveValueComponents(path(input),path(output));ASSERT_EQ(r.grids.size(),3u);EXPECT_EQ(r.activeComponentCount,3u);EXPECT_EQ(r.sourceActiveVoxels,4u);EXPECT_FALSE(r.emptySourceCopy);EXPECT_EQ(r.grids[0].activeVoxelCount,2u);EXPECT_EQ(r.grids[1].activeVoxelCount,1u);EXPECT_EQ(r.grids[2].activeVoxelCount,1u);EXPECT_EQ(r.grids[0].activeBounds->minimum,(std::array<int32_t,3>{-9,0,0}));EXPECT_EQ(r.aggregateOutputLeafVoxels,r.expandedStoredLeafVoxels*3);
    for(size_t i=0;i<r.grids.size();++i){const auto name="component_00000"+std::to_string(i+1);EXPECT_EQ(r.grids[i].gridName,name);const auto outputGrid=read<Grid>(path(output),name);ASSERT_TRUE(outputGrid);metadataChecks(*outputGrid);EXPECT_FALSE(outputGrid->isInWorldSpace());EXPECT_EQ(outputGrid->getVectorType(),openvdb::VEC_CONTRAVARIANT_ABSOLUTE);EXPECT_EQ(outputGrid->getGridClass(),source->getGridClass());
      for(size_t j=0;j<points.size();++j){const bool selected=i==(j<2?0:j-1);EXPECT_EQ(outputGrid->tree().isValueOn(points[j]),selected);if constexpr(std::is_same_v<Grid,openvdb::MaskGrid>)EXPECT_EQ(outputGrid->tree().getValue(points[j]),selected);else sameBits(outputGrid->tree().getValue(points[j]),source->tree().getValue(points[j]));}
      for(const auto& c:{inactive,openvdb::Coord(8192),openvdb::Coord(12345678)}){EXPECT_FALSE(outputGrid->tree().isValueOn(c));if constexpr(!std::is_same_v<Grid,openvdb::MaskGrid>)sameBits(outputGrid->tree().getValue(c),source->tree().getValue(c));}sameBits(outputGrid->background(),source->background());
    }checkFileMetadata(output);
  }
};
TEST_F(OpenVdbValueComponents, AllNineTypesKeepSourceFieldBitsAndOnlyChangeComponentActivity){
  typed<openvdb::BoolGrid>("bool.vdb","bool-out.vdb",true,false,true);
  typed<openvdb::MaskGrid>("mask.vdb","mask-out.vdb",false,true,false);
  typed<openvdb::Int32Grid>("i32.vdb","i32-out.vdb",-5,INT32_MIN,INT32_MAX);
  typed<openvdb::Int64Grid>("i64.vdb","i64-out.vdb",INT64_MIN,INT64_C(9007199254740993),INT64_MAX);
  typed<openvdb::FloatGrid>("f.vdb","f-out.vdb",0.f,std::bit_cast<float>(uint32_t(0x7f812345)),-0.f);
  typed<openvdb::DoubleGrid>("d.vdb","d-out.vdb",0.,std::bit_cast<double>(uint64_t(0xfff0000000001234)),-0.);
  typed<openvdb::Vec3IGrid>("vi.vdb","vi-out.vdb",openvdb::Vec3i(0),openvdb::Vec3i(INT32_MIN,0,INT32_MAX),openvdb::Vec3i(9,-3,7));
  typed<openvdb::Vec3SGrid>("vs.vdb","vs-out.vdb",openvdb::Vec3s(0),openvdb::Vec3s(-0.f,std::bit_cast<float>(uint32_t(0x7f812345)),2.f),openvdb::Vec3s(1,-0.f,-3));
  typed<openvdb::Vec3DGrid>("vd.vdb","vd-out.vdb",openvdb::Vec3d(0),openvdb::Vec3d(-0.,std::bit_cast<double>(uint64_t(0x7ff0000000001234)),2.),openvdb::Vec3d(1,-0.,-3));
}
TEST_F(OpenVdbValueComponents, ActiveTileAndLeafBoundaryJoinWithoutLosingInactiveContinuations){
  auto g=openvdb::DoubleGrid::create(4.);stamp(*g);g->setGridClass(openvdb::GRID_LEVEL_SET);g->tree().addTile(1,openvdb::Coord(0),3.25,true);raw(*g,openvdb::Coord(8,0,0),-0.);raw(*g,openvdb::Coord(16,0,0),2.);raw(*g,openvdb::Coord(1000,0,0),-4.,false);write<openvdb::DoubleGrid>("tiles.vdb",g);
  const auto r=vdb::WriteActiveValueComponents(path("tiles.vdb"),path("tiles-out.vdb"));ASSERT_EQ(r.grids.size(),2u);EXPECT_EQ(r.sourceActiveVoxels,514u);EXPECT_EQ(r.sourceActiveTiles,1u);EXPECT_EQ(r.expandedStoredLeafVoxels,r.sourceStoredLeafVoxels+512);EXPECT_EQ(r.grids[0].activeVoxelCount,513u);EXPECT_EQ(r.grids[1].activeVoxelCount,1u);
  const auto a=read<openvdb::DoubleGrid>(path("tiles-out.vdb"),r.grids[0].gridName),b=read<openvdb::DoubleGrid>(path("tiles-out.vdb"),r.grids[1].gridName);for(int z=0;z<8;++z)for(int y=0;y<8;++y)for(int x=0;x<8;++x){const openvdb::Coord c(x,y,z);EXPECT_TRUE(a->tree().isValueOn(c));EXPECT_FALSE(b->tree().isValueOn(c));EXPECT_EQ(a->tree().getValue(c),3.25);EXPECT_EQ(b->tree().getValue(c),3.25);}EXPECT_EQ(std::bit_cast<uint64_t>(b->tree().getValue(openvdb::Coord(8,0,0))),0x8000000000000000ull);EXPECT_EQ(b->tree().getValue(openvdb::Coord(1000,0,0)),-4.);EXPECT_EQ(a->getGridClass(),openvdb::GRID_LEVEL_SET);
  auto cap=vdb::ActiveValueComponentsOptions{};cap.maxStoredLeafVoxels=r.sourceStoredLeafVoxels;
  EXPECT_THROW((void)vdb::WriteActiveValueComponents(path("tiles.vdb"),path("growth-fail.vdb"),cap),std::invalid_argument);
  EXPECT_FALSE(std::filesystem::exists(path("growth-fail.vdb")));
}
TEST_F(OpenVdbValueComponents, RemoteInt32EndpointsNeverWrapNeighborsOrAllocateTheirBoundingBox){
  auto g=openvdb::Int64Grid::create(0);stamp(*g);raw(*g,openvdb::Coord(INT32_MIN),INT64_MIN);raw(*g,openvdb::Coord(INT32_MIN+1,INT32_MIN,INT32_MIN),INT64_MAX);raw(*g,openvdb::Coord(INT32_MAX),INT64_C(9007199254740993));write<openvdb::Int64Grid>("endpoints.vdb",g);
  const auto r=vdb::WriteActiveValueComponents(path("endpoints.vdb"),path("endpoints-out.vdb"));ASSERT_EQ(r.grids.size(),2u);EXPECT_EQ(r.grids[0].activeVoxelCount,2u);EXPECT_EQ(r.grids[1].activeVoxelCount,1u);EXPECT_EQ(r.grids[0].activeBounds->minimum,(std::array<int32_t,3>{INT32_MIN,INT32_MIN,INT32_MIN}));EXPECT_EQ(r.grids[1].activeBounds->maximum,(std::array<int32_t,3>{INT32_MAX,INT32_MAX,INT32_MAX}));
}
TEST_F(OpenVdbValueComponents, EmptyActivityCopiesAllInactiveStorageAndBackgroundWithExplicitZeroComponents){
  auto g=openvdb::DoubleGrid::create(-0.);stamp(*g);raw(*g,openvdb::Coord(-9,0,0),std::bit_cast<double>(uint64_t(0x7ff0000000001234)),false);g->tree().addTile(3,openvdb::Coord(4096),-7.,false);write<openvdb::DoubleGrid>("empty.vdb",g);
  const auto r=vdb::WriteActiveValueComponents(path("empty.vdb"),path("empty-out.vdb"));EXPECT_TRUE(r.emptySourceCopy);EXPECT_EQ(r.activeComponentCount,0u);ASSERT_EQ(r.grids.size(),1u);EXPECT_EQ(r.grids[0].gridName,"component_000000");EXPECT_FALSE(r.grids[0].activeBounds);const auto out=read<openvdb::DoubleGrid>(path("empty-out.vdb"),r.grids[0].gridName);EXPECT_EQ(out->activeVoxelCount(),0u);EXPECT_EQ(std::bit_cast<uint64_t>(out->background()),0x8000000000000000ull);EXPECT_EQ(std::bit_cast<uint64_t>(out->tree().getValue(openvdb::Coord(-9,0,0))),0x7ff0000000001234ull);EXPECT_EQ(out->tree().getValue(openvdb::Coord(4096)),-7.);metadataChecks(*out);checkFileMetadata("empty-out.vdb");
}
TEST_F(OpenVdbValueComponents, ExactTinyTranslatedAndNonlinearMapsRemainMetadataWithoutWorldResampling){
  for(bool nonlinear:{false,true}){auto g=openvdb::FloatGrid::create(0.f);stamp(*g);raw(*g,openvdb::Coord(0),1.f);raw(*g,openvdb::Coord(3,0,0),2.f);if(nonlinear)g->setTransform(openvdb::math::Transform::createFrustumTransform(openvdb::BBoxd(openvdb::Vec3d(-2),openvdb::Vec3d(2)),.75,5.));else{std::stringstream record(std::ios::binary|std::ios::in|std::ios::out);openvdb::writeString(record,openvdb::math::UniformScaleTranslateMap::mapType());openvdb::Vec3d(1e9+.125,-1e9,3).write(record);for(double n:{1e-6,1e-6,1e6,1e12,5e5})openvdb::Vec3d(n).write(record);record.seekg(0);openvdb::io::setCurrentVersion(record);auto transform=std::make_shared<openvdb::math::Transform>();transform->read(record);g->setTransform(transform);}const auto input=nonlinear?"frustum.vdb":"tiny.vdb",output=nonlinear?"frustum-out.vdb":"tiny-out.vdb";write<openvdb::FloatGrid>(input,g);const auto r=vdb::WriteActiveValueComponents(path(input),path(output));ASSERT_EQ(r.grids.size(),2u);const auto out=read<openvdb::FloatGrid>(path(output),r.grids[0].gridName);EXPECT_EQ(out->transform().mapType(),g->transform().mapType());std::ostringstream before(std::ios::binary),after(std::ios::binary);g->transform().write(before);out->transform().write(after);EXPECT_EQ(before.str(),after.str());}
}
TEST_F(OpenVdbValueComponents, ComponentAndAggregateOutputCapsFailBeforePublicationAndProtectExistingFiles){
  auto g=openvdb::Int64Grid::create(0);stamp(*g);raw(*g,openvdb::Coord(0),INT64_MIN);raw(*g,openvdb::Coord(2,2,0),INT64_MAX);write<openvdb::Int64Grid>("caps.vdb",g);const auto r=vdb::WriteActiveValueComponents(path("caps.vdb"),path("caps-out.vdb"));
  std::vector<vdb::ActiveValueComponentsOptions> caps(9);caps[0].maxComponents=1;caps[1].maxActiveVoxels=1;caps[2].maxAggregateOutputLeafVoxels=r.aggregateOutputLeafVoxels-1;caps[3].maxNodes=r.nodeUpperBound-1;caps[4].maxInputBytes=r.workspaceUpperBound-1;caps[5].maxWork=r.workUpperBound-1;caps[6].maxOutputBytes=1;caps[7].maxMetadataBytes=1;caps[8].maxStoredLeafVoxels=1;
  for(size_t i=0;i<caps.size();++i){const auto output=path(("reject-"+std::to_string(i)+".vdb").c_str());EXPECT_THROW((void)vdb::WriteActiveValueComponents(path("caps.vdb"),output,caps[i]),std::invalid_argument);EXPECT_FALSE(std::filesystem::exists(output));}
  auto huge=openvdb::BoolGrid::create(false);stamp(*huge);huge->tree().addTile(3,openvdb::Coord(0),false,true);write<openvdb::BoolGrid>("huge.vdb",huge);
  EXPECT_THROW((void)vdb::WriteActiveValueComponents(path("huge.vdb"),path("huge-out.vdb")),std::invalid_argument);
  EXPECT_FALSE(std::filesystem::exists(path("huge-out.vdb")));
  {std::ofstream foreign(path("foreign.vdb"),std::ios::binary);foreign.write("foreign\0bytes",13);}
  const auto foreign=contents(path("foreign.vdb"));
  EXPECT_THROW((void)vdb::WriteActiveValueComponents(path("caps.vdb"),path("foreign.vdb")),std::invalid_argument);
  EXPECT_EQ(contents(path("foreign.vdb")),foreign);
  EXPECT_THROW((void)vdb::WriteActiveValueComponents(path("caps.vdb"),path("caps.vdb")),std::invalid_argument);
}
TEST_F(OpenVdbValueComponents, UnicodeParentsExclusiveOutputAndMandatoryDelayedLoading){
  auto g=openvdb::DoubleGrid::create(0);stamp(*g);raw(*g,openvdb::Coord(0),-0.);write<openvdb::DoubleGrid>("seed.vdb",g);const auto parent=directory/std::filesystem::path(u8"入力-é");ASSERT_TRUE(std::filesystem::create_directory(parent));const auto input=parent/std::filesystem::path(u8"元.vdb"),output=parent/std::filesystem::path(u8"成分.vdb");std::filesystem::copy_file(path("seed.vdb"),input);const auto original=contents(input);const auto result=vdb::WriteActiveValueComponents(input,output);EXPECT_EQ(result.activeComponentCount,1u);const auto done=contents(output);
  EXPECT_THROW((void)vdb::WriteActiveValueComponents(input,output),std::invalid_argument);
  EXPECT_EQ(contents(input),original);EXPECT_EQ(contents(output),done);
  struct Environment {std::string previous;bool existed=false;Environment(){const char* p=std::getenv("OPENVDB_DISABLE_DELAYED_LOAD");if(p){previous=p;existed=true;}
#ifdef _WIN32
    _putenv_s("OPENVDB_DISABLE_DELAYED_LOAD","1");
#else
    setenv("OPENVDB_DISABLE_DELAYED_LOAD","1",1);
#endif
  }~Environment(){
#ifdef _WIN32
    _putenv_s("OPENVDB_DISABLE_DELAYED_LOAD",existed?previous.c_str():"");
#else
    if(existed)setenv("OPENVDB_DISABLE_DELAYED_LOAD",previous.c_str(),1);else unsetenv("OPENVDB_DISABLE_DELAYED_LOAD");
#endif
  }} environment;
  try{(void)vdb::WriteActiveValueComponents(input,parent/"disabled.vdb");FAIL();}catch(const std::invalid_argument& e){EXPECT_NE(std::string(e.what()).find("delayed loading"),std::string::npos);}EXPECT_FALSE(std::filesystem::exists(parent/"disabled.vdb"));
}
TEST_F(OpenVdbValueComponents, StrictCatalogSelectionRecordedStatisticsAndOptionRangesProtectSource){
  auto first=openvdb::Int64Grid::create(0),second=openvdb::Int64Grid::create(0);stamp(*first);stamp(*second);raw(*first,openvdb::Coord(0),INT64_MIN);raw(*second,openvdb::Coord(3,0,0),INT64_MAX);
  {openvdb::io::File file(path("duplicates.vdb").string());file.write({std::make_shared<ExactFixture<openvdb::Int64Grid>>(*first),std::make_shared<ExactFixture<openvdb::Int64Grid>>(*second)},fileMetadata());file.close();}
  originals.emplace_back(path("duplicates.vdb"),contents(path("duplicates.vdb")));
  EXPECT_THROW((void)vdb::WriteActiveValueComponents(path("duplicates.vdb"),path("ambiguous.vdb")),std::invalid_argument);
  EXPECT_THROW((void)vdb::WriteActiveValueComponents(path("duplicates.vdb"),path("bare.vdb"),{},"source"),std::invalid_argument);
  const auto descriptors=names(path("duplicates.vdb"));ASSERT_EQ(descriptors.size(),2u);
  const auto result=vdb::WriteActiveValueComponents(path("duplicates.vdb"),path("chosen.vdb"),{},"descriptor:"+descriptors[0]);EXPECT_EQ(result.sourceActiveVoxels,1u);
  auto cap=vdb::ActiveValueComponentsOptions{};cap.maxGrids=1;
  EXPECT_THROW((void)vdb::WriteActiveValueComponents(path("duplicates.vdb"),path("catalog-fail.vdb"),cap,"descriptor:"+descriptors[0]),std::invalid_argument);
  first->removeMeta(openvdb::GridBase::META_FILE_MEM_BYTES);
  {openvdb::io::File file(path("no-stats.vdb").string());file.setGridStatsMetadataEnabled(false);file.write({std::make_shared<ExactFixture<openvdb::Int64Grid>>(*first)},fileMetadata());file.close();}
  EXPECT_THROW((void)vdb::WriteActiveValueComponents(path("no-stats.vdb"),path("no-stats-out.vdb")),std::invalid_argument);
  std::vector<vdb::ActiveValueComponentsOptions> invalid(5);invalid[0].maxComponents=0;invalid[1].maxComponents=65;invalid[2].gridNamePrefix.clear();invalid[3].maxInputBytes=(16ull<<30)+1;invalid[4].maxWork=10'000'000'001ull;
  for(const auto& o:invalid)EXPECT_THROW((void)vdb::WriteActiveValueComponents(path("missing"),path("invalid.vdb"),o),std::invalid_argument);
  EXPECT_FALSE(std::filesystem::exists(path("ambiguous.vdb")));EXPECT_FALSE(std::filesystem::exists(path("bare.vdb")));EXPECT_FALSE(std::filesystem::exists(path("catalog-fail.vdb")));EXPECT_FALSE(std::filesystem::exists(path("no-stats-out.vdb")));EXPECT_FALSE(std::filesystem::exists(path("invalid.vdb")));
}
#endif
} // namespace
