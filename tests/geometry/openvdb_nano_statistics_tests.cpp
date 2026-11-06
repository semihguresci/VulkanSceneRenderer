#include "Container/geometry/OpenVdbNanoStatistics.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>
#include <gtest/gtest.h>
#if defined(CONTAINER_HAS_NANOVDB_IO) && CONTAINER_HAS_NANOVDB_IO
#include "Container/geometry/detail/OpenVdbNanoCompatibility.h"
#include <nanovdb/io/IO.h>
#include <nanovdb/tools/CreateNanoGrid.h>
#include <nanovdb/tools/GridChecksum.h>
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
#include <openvdb/openvdb.h>
#include <openvdb/io/File.h>
#endif
#endif
namespace {
namespace vdb=container::geometry::vdb;
TEST(OpenVdbNanoStatisticsAvailability, RequiredFeaturePrecedesInvalidPathsAndOptions) {
#if !defined(CONTAINER_HAS_NANOVDB_IO) || !CONTAINER_HAS_NANOVDB_IO
  vdb::NanoVdbLimits limits; limits.maxWork=0;
  try {
    (void)vdb::WriteNanoVdbStatistics("missing","missing",static_cast<vdb::NanoVdbStatisticsMode>(99),{},{},limits);
    FAIL();
  } catch (const std::runtime_error& e) {
    EXPECT_NE(std::string(e.what()).find("ENABLE_NANOVDB_IO"),std::string::npos);
  }
#else
  EXPECT_TRUE(vdb::NanoVdbInterchangeCapabilities().nativeIo);
#endif
}
#if defined(CONTAINER_HAS_NANOVDB_IO) && CONTAINER_HAS_NANOVDB_IO
namespace nv=nanovdb;
class OpenVdbNanoStatistics:public ::testing::Test {
protected:
  std::filesystem::path directory;
  void SetUp() override {
    static std::atomic_uint64_t serial{};
    directory=std::filesystem::temp_directory_path()/
      ("container-nano-stats-test-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+"-"+std::to_string(++serial));
    ASSERT_TRUE(std::filesystem::create_directory(directory));
  }
  void TearDown() override {
    std::error_code e;
    if(std::filesystem::is_symlink(directory,e) || e) return;
    const auto resolved=std::filesystem::weakly_canonical(directory,e);
    if(e) return;
    const auto parent=std::filesystem::weakly_canonical(std::filesystem::temp_directory_path(),e);
    if(!e && resolved.parent_path()==parent && resolved.filename()==directory.filename() &&
       resolved.filename().string().starts_with("container-nano-stats-test-"))
      std::filesystem::remove_all(resolved,e);
  }
  std::filesystem::path path(const char* name) const { return directory/name; }
  template<class T> auto values(T background,std::initializer_list<T> values,const std::string& name="known") {
    nv::tools::build::Grid<T> source(background,name);
    auto accessor=source.getAccessor();
    int i=-1;
    for(const auto& value:values) accessor.setValue(nv::Coord(i++,0,0),value);
    return nv::tools::createNanoGrid(source,nv::tools::StatsMode::Disable,nv::CheckMode::Full);
  }
  template<class H> void raw(const char* name,const H& handle) {
    std::ofstream stream(path(name),std::ios::binary);
    stream.write(static_cast<const char*>(handle.data()),std::streamsize(handle.size()));
    ASSERT_TRUE(stream.good());
  }
  template<class T> auto read(const char* name) {
    std::ifstream stream(path(name),std::ios::binary);
    return nv::io::readGrid<nv::HostBuffer>(stream,0,nv::HostBuffer{});
  }
};
TEST_F(OpenVdbNanoStatistics, DoubleCachesBoundsAndMomentsMatchIndependentValues) {
  auto source=values<double>(-0.,{1.,3.,5.},std::string(300,'s'));
  raw("source.nvdb",source);
  const auto result=vdb::WriteNanoVdbStatistics(path("source.nvdb"),path("updated.nvdb"));
  ASSERT_EQ(result.summaries.size(),1u);
  const auto& s=result.summaries[0];
  EXPECT_EQ(s.name,std::string(300,'s'));
  EXPECT_EQ(s.activeVoxels,3u);
  EXPECT_EQ(s.indexMinimum,(std::array<int64_t,3>{-1,0,0}));
  EXPECT_EQ(s.indexMaximum,(std::array<int64_t,3>{1,0,0}));
  ASSERT_TRUE(s.minimum); ASSERT_TRUE(s.maximum);
  EXPECT_EQ(std::get<double>(*s.minimum),1.); EXPECT_EQ(std::get<double>(*s.maximum),5.);
  EXPECT_NEAR(*s.average,3.,1e-12);
  EXPECT_NEAR(*s.standardDeviation,std::sqrt(8./3),1e-12);
  EXPECT_GT(result.work,result.file.work);
  EXPECT_TRUE(vdb::ValidateNanoVdbFile(path("updated.nvdb")).grids[0].checksumVerified);
  auto independent=read<double>("updated.nvdb");
  const auto* grid=independent.grid<double>(); ASSERT_TRUE(grid);
  EXPECT_EQ(grid->tree().getValue(nv::Coord(1,0,0)),5.);
  EXPECT_TRUE(std::signbit(grid->tree().background()));
  EXPECT_EQ(std::memcmp(&grid->mMap,&source.grid<double>()->mMap,sizeof(nv::Map)),0);
}
TEST_F(OpenVdbNanoStatistics, ExactInt64ExtremaAndRequestedCacheModesRemainDistinct) {
  auto source=values<int64_t>(INT64_MIN,{INT64_MAX-2,INT64_MAX-1,INT64_MAX});
  raw("source.nvdb",source);
  for(const auto mode:{vdb::NanoVdbStatisticsMode::Bounds,vdb::NanoVdbStatisticsMode::MinMax,vdb::NanoVdbStatisticsMode::All}) {
    const auto output=path(mode==vdb::NanoVdbStatisticsMode::Bounds ? "bounds.nvdb" : mode==vdb::NanoVdbStatisticsMode::MinMax ? "extrema.nvdb" : "all.nvdb");
    const auto result=vdb::WriteNanoVdbStatistics(path("source.nvdb"),output,mode);
    const auto& s=result.summaries[0];
    EXPECT_EQ(s.minimum.has_value(),mode!=vdb::NanoVdbStatisticsMode::Bounds);
    EXPECT_EQ(s.average.has_value(),mode==vdb::NanoVdbStatisticsMode::All);
    if(s.minimum) EXPECT_EQ(std::get<int64_t>(*s.minimum),INT64_MAX-2);
    if(s.maximum) EXPECT_EQ(std::get<int64_t>(*s.maximum),INT64_MAX);
  }
  auto integers=values<int32_t>(0,{INT32_MIN,-1,INT32_MAX});
  raw("integers.nvdb",integers);
  const auto integersResult=vdb::WriteNanoVdbStatistics(path("integers.nvdb"),path("integers-out.nvdb"));
  EXPECT_EQ(std::get<int32_t>(*integersResult.summaries[0].minimum),INT32_MIN);
  EXPECT_EQ(std::get<int32_t>(*integersResult.summaries[0].maximum),INT32_MAX);
}
TEST_F(OpenVdbNanoStatistics, VectorExtremaAndMomentsDescribeSquaredMagnitudes) {
  auto source=values<nv::Vec3d>(nv::Vec3d(0),{nv::Vec3d(3,4,0),nv::Vec3d(0,0,2),nv::Vec3d(0,0,9)});
  raw("source.nvdb",source);
  const auto result=vdb::WriteNanoVdbStatistics(path("source.nvdb"),path("updated.nvdb"));
  const auto& s=result.summaries[0];
  ASSERT_TRUE(s.minimum); ASSERT_TRUE(s.maximum); ASSERT_TRUE(s.average);
  EXPECT_EQ((std::get<std::array<double,3>>(*s.minimum)),(std::array<double,3>{0,0,2}));
  EXPECT_EQ((std::get<std::array<double,3>>(*s.maximum)),(std::array<double,3>{0,0,9}));
  EXPECT_NEAR(*s.average,110./3,1e-12);
  EXPECT_NEAR(*s.standardDeviation,std::sqrt(9506./9),1e-12);
}
TEST_F(OpenVdbNanoStatistics, InvalidCachesAreRecomputedWithoutChangingSourceOrStoredValues) {
  auto source=values<float>(-0.f,{1.f,3.f,5.f});
  auto* grid=source.grid<float>();
  grid->tree().root().data()->mMinimum=std::numeric_limits<float>::quiet_NaN();
  grid->setMinMaxOn(true);
  nv::tools::updateChecksum(grid->data(),nv::CheckMode::Full);
  raw("source.nvdb",source);
  const auto result=vdb::WriteNanoVdbStatistics(path("source.nvdb"),path("updated.nvdb"));
  auto updated=read<float>("updated.nvdb");
  EXPECT_EQ(updated.grid<float>()->tree().root().minimum(),1.f);
  EXPECT_EQ(updated.grid<float>()->tree().root().maximum(),5.f);
  auto original=read<float>("source.nvdb");
  EXPECT_TRUE(std::isnan(original.grid<float>()->tree().root().minimum()));
  for(int i=-2;i<4;++i) {
    const nv::Coord ijk(i,0,0);
    EXPECT_EQ(updated.grid<float>()->tree().getValue(ijk),source.grid<float>()->tree().getValue(ijk));
    EXPECT_EQ(updated.grid<float>()->tree().isActive(ijk),source.grid<float>()->tree().isActive(ijk));
  }
}
TEST_F(OpenVdbNanoStatistics, EmptyAndMaskGridsHaveExplicitlyMissingNumericSummaries) {
  auto empty=values<double>(2.,{});
  raw("empty.nvdb",empty);
  const auto result=vdb::WriteNanoVdbStatistics(path("empty.nvdb"),path("empty-out.nvdb"));
  EXPECT_FALSE(result.summaries[0].indexMinimum);
  EXPECT_FALSE(result.summaries[0].minimum); EXPECT_FALSE(result.summaries[0].average);
  nv::tools::build::Grid<nv::ValueMask> source(false,"mask",nv::GridClass::Topology);
  source.getAccessor().setValue(nv::Coord(-1,0,0),true);
  auto mask=nv::tools::createNanoGrid(source);
  raw("mask.nvdb",mask);
  const auto m=vdb::WriteNanoVdbStatistics(path("mask.nvdb"),path("mask-out.nvdb"));
  EXPECT_EQ(m.summaries[0].activeVoxels,1u);
  EXPECT_TRUE(m.summaries[0].indexMinimum);
  EXPECT_FALSE(m.summaries[0].minimum); EXPECT_FALSE(m.summaries[0].average);
  // The pinned builder's Bool leaf aliases its BuildType to ValueMask. Build
  // an empty tree with the shared fixed-size root layout, then explicitly tag
  // and initialize its Boolean background; no leaf representation is reused.
  static_assert(sizeof(nv::NanoRoot<bool>::DataType)==sizeof(nv::NanoRoot<nv::ValueMask>::DataType));
  nv::tools::build::Grid<nv::ValueMask> boolean(false,"boolean");
  auto booleanHandle=nv::tools::createNanoGrid(boolean);
  auto* booleanHeader=static_cast<nv::GridData*>(booleanHandle.buffer().data());
  booleanHeader->mGridType=nv::GridType::Boolean;
  booleanHeader->mGridClass=nv::GridClass::Unknown;
  auto* booleanRoot=reinterpret_cast<nv::NanoRoot<bool>::DataType*>(
      static_cast<uint8_t*>(booleanHandle.buffer().data())+sizeof(nv::GridData)+sizeof(nv::TreeData));
  booleanRoot->mBackground=true;
  nv::tools::updateChecksum(booleanHeader,nv::CheckMode::Full);
  raw("boolean.nvdb",booleanHandle);
  const auto b=vdb::WriteNanoVdbStatistics(path("boolean.nvdb"),path("boolean-out.nvdb"));
  EXPECT_EQ(b.summaries[0].valueType,"bool");
  EXPECT_FALSE(b.summaries[0].minimum); EXPECT_FALSE(b.summaries[0].average);
}

TEST_F(OpenVdbNanoStatistics, ActiveTilesAreWeightedAndInactiveBranchesKeepTheirValues) {
  nv::tools::build::Grid<double> source(100.,"weighted");
  source.tree().root().addTile<1>(nv::Coord(0),100.,false);
  source.tree().root().addTile<1>(nv::Coord(0),2.,true);
  source.tree().root().addTile<1>(nv::Coord(8,0,0),6.,true);
  source.tree().root().addTile<1>(nv::Coord(8192,0,0),-7.,false);
  auto handle=nv::tools::createNanoGrid(source,nv::tools::StatsMode::Disable,nv::CheckMode::Full);
  raw("source.nvdb",handle);
  const auto result=vdb::WriteNanoVdbStatistics(path("source.nvdb"),path("updated.nvdb"));
  ASSERT_EQ(result.summaries[0].activeVoxels,1024u);
  EXPECT_DOUBLE_EQ(*result.summaries[0].average,4.);
  EXPECT_DOUBLE_EQ(*result.summaries[0].standardDeviation,2.);
  auto updated=read<double>("updated.nvdb");
  EXPECT_EQ(updated.grid<double>()->tree().getValue(nv::Coord(8193,1,1)),-7.);
  EXPECT_FALSE(updated.grid<double>()->tree().isActive(nv::Coord(8193,1,1)));
  EXPECT_EQ(updated.grid<double>()->tree().getValue(nv::Coord(99999)),100.);
}

TEST_F(OpenVdbNanoStatistics, ChecksumOptInNeverPermitsInvalidPointerGraphs) {
  auto source=values<float>(0.f,{1.f,3.f,5.f});
  auto* header=static_cast<nv::GridData*>(source.buffer().data());
  header->mGridName[0]='K';
  raw("mismatch.nvdb",source);
  EXPECT_THROW((void)vdb::WriteNanoVdbStatistics(path("mismatch.nvdb"),path("bad.nvdb")),std::invalid_argument);
  vdb::NanoVdbReadOptions readOptions; readOptions.verifyChecksums=false;
  const auto repaired=vdb::WriteNanoVdbStatistics(path("mismatch.nvdb"),path("repaired.nvdb"),{},readOptions);
  EXPECT_TRUE(std::any_of(repaired.diagnostics.begin(),repaired.diagnostics.end(),[](const auto& s) {
    return s.find("CRC mismatch accepted")!=std::string::npos;
  }));
  auto* tree=reinterpret_cast<nv::TreeData*>(static_cast<uint8_t*>(source.buffer().data())+sizeof(nv::GridData));
  tree->mNodeOffset[0]=INT64_MAX;
  raw("structural.nvdb",source);
  EXPECT_THROW((void)vdb::WriteNanoVdbStatistics(path("structural.nvdb"),path("bad.nvdb"),{},readOptions),std::invalid_argument);
  EXPECT_FALSE(std::filesystem::exists(path("bad.nvdb")));
}

TEST_F(OpenVdbNanoStatistics, LastInt32VoxelRejectsUpstreamExclusiveBoundOverflow) {
  nv::tools::build::Grid<float> source(0.f,"edge");
  source.getAccessor().setValue(nv::Coord(INT32_MAX,0,0),1.f);
  auto handle=nv::tools::createNanoGrid(source,nv::tools::StatsMode::Disable,nv::CheckMode::Full);
  raw("edge.nvdb",handle);
  EXPECT_NO_THROW((void)vdb::ValidateNanoVdbFile(path("edge.nvdb")));
  EXPECT_THROW((void)vdb::WriteNanoVdbStatistics(path("edge.nvdb"),path("bad.nvdb")),std::invalid_argument);
  EXPECT_FALSE(std::filesystem::exists(path("bad.nvdb")));
}
TEST_F(OpenVdbNanoStatistics, UnsafeNumericRangesRejectBeforePublication) {
  auto source=values<double>(0.,{1e300,-1e300});
  raw("huge.nvdb",source);
  EXPECT_THROW((void)vdb::WriteNanoVdbStatistics(path("huge.nvdb"),path("bad.nvdb")),std::invalid_argument);
  EXPECT_FALSE(std::filesystem::exists(path("bad.nvdb")));
  EXPECT_NO_THROW((void)vdb::WriteNanoVdbStatistics(path("huge.nvdb"),path("minmax.nvdb"),vdb::NanoVdbStatisticsMode::MinMax));
  auto vector=values<nv::Vec3f>(nv::Vec3f(0),{nv::Vec3f(1e30f)});
  raw("vector.nvdb",vector);
  EXPECT_THROW((void)vdb::WriteNanoVdbStatistics(path("vector.nvdb"),path("bad.nvdb"),vdb::NanoVdbStatisticsMode::MinMax),std::invalid_argument);
  EXPECT_NO_THROW((void)vdb::WriteNanoVdbStatistics(path("vector.nvdb"),path("bounds.nvdb"),vdb::NanoVdbStatisticsMode::Bounds));
}
TEST_F(OpenVdbNanoStatistics, SharedLimitsAndExistingPathsLeaveSourceIntact) {
  auto source=values<float>(0.f,{1.f,3.f,5.f}); raw("source.nvdb",source);
  for(int boundary=0;boundary<4;++boundary) {
    vdb::NanoVdbLimits limits;
    if(boundary==0) limits.maxWork=1;
    if(boundary==1) limits.maxWorkspaceBytes=1;
    if(boundary==2) limits.maxNativeBytes=1;
    if(boundary==3) limits.maxNodes=1;
    EXPECT_THROW((void)vdb::WriteNanoVdbStatistics(path("source.nvdb"),path("bad.nvdb"),{}, {},{},limits),std::invalid_argument);
    EXPECT_FALSE(std::filesystem::exists(path("bad.nvdb")));
  }
  EXPECT_THROW((void)vdb::WriteNanoVdbStatistics(path("source.nvdb"),path("source.nvdb")),std::invalid_argument);
  std::ifstream f(path("source.nvdb"),std::ios::binary);
  const std::string bytes(std::istreambuf_iterator<char>(f),{});
  ASSERT_EQ(bytes.size(),source.size());
  EXPECT_EQ(std::memcmp(bytes.data(),source.data(),bytes.size()),0);
}
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
TEST_F(OpenVdbNanoStatistics, NonemptyBooleanLeavesAndVdbMetadataSurviveRecomputation) {
  openvdb::initialize();
  auto source=openvdb::BoolGrid::create(true); source->setName("boolean");
  source->tree().setValueOn(openvdb::Coord(-9,2,3),false);
  source->tree().setValueOn(openvdb::Coord(1,2,3),true);
  source->tree().setValueOff(openvdb::Coord(8001,2,3),false);
  source->insertMeta("exact-id",openvdb::Int64Metadata(INT64_MAX-1));
  { openvdb::io::File file(path("source.vdb").string()); file.write(openvdb::GridPtrVec{source}); }
  (void)vdb::WriteNanoVdbFromVdb(path("source.vdb"),path("source.nvdb"));
  const auto result=vdb::WriteNanoVdbStatistics(path("source.nvdb"),path("updated.nvdb"));
  ASSERT_EQ(result.summaries[0].activeVoxels,2u);
  EXPECT_FALSE(result.summaries[0].minimum); EXPECT_FALSE(result.summaries[0].average);
  (void)vdb::WriteVdbFromNanoVdb(path("updated.nvdb"),path("restored.vdb"));
  openvdb::io::File file(path("restored.vdb").string()); file.open(false);
  const auto restored=openvdb::gridPtrCast<openvdb::BoolGrid>(file.readGrid("boolean"));
  ASSERT_TRUE(restored);
  EXPECT_EQ(restored->metaValue<int64_t>("exact-id"),INT64_MAX-1);
  EXPECT_FALSE(restored->tree().getValue(openvdb::Coord(-9,2,3)));
  EXPECT_TRUE(restored->tree().isValueOn(openvdb::Coord(-9,2,3)));
  EXPECT_FALSE(restored->tree().getValue(openvdb::Coord(8001,2,3)));
  EXPECT_FALSE(restored->tree().isValueOn(openvdb::Coord(8001,2,3)));
}
#endif
#endif
} // namespace
