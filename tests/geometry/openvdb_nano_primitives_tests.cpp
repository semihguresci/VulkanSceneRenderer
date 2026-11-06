#include "Container/geometry/OpenVdbNanoPrimitives.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>
#include <gtest/gtest.h>
#if defined(CONTAINER_HAS_NANOVDB_IO) && CONTAINER_HAS_NANOVDB_IO
#include <nanovdb/io/IO.h>
#endif
namespace {
namespace vdb=container::geometry::vdb;
TEST(OpenVdbNanoPrimitivesAvailability, RequiredFeaturePrecedesInvalidOptionsAndPaths) {
#if !defined(CONTAINER_HAS_NANOVDB_IO) || !CONTAINER_HAS_NANOVDB_IO
  vdb::NanoVdbLimits limits;limits.maxWork=0;
  try {(void)vdb::WriteNanoVdbPrimitive("missing/path",static_cast<vdb::NanoVdbPrimitiveShape>(99),{},{},limits);FAIL();}
  catch(const std::runtime_error& e){EXPECT_NE(std::string(e.what()).find("ENABLE_NANOVDB_IO"),std::string::npos);}
#else
  EXPECT_TRUE(vdb::NanoVdbInterchangeCapabilities().nativeIo);
#if !defined(CONTAINER_HAS_OPENVDB) || !CONTAINER_HAS_OPENVDB
  EXPECT_FALSE(vdb::NanoVdbInterchangeCapabilities().openVdbConversion);
#endif
#endif
}
#if defined(CONTAINER_HAS_NANOVDB_IO) && CONTAINER_HAS_NANOVDB_IO
namespace nv=nanovdb;
class OpenVdbNanoPrimitives:public ::testing::Test {
protected:
  std::filesystem::path directory;
  void SetUp()override{static std::atomic_uint64_t serial{};directory=std::filesystem::temp_directory_path()/("container-nano-primitives-test-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+"-"+std::to_string(++serial));ASSERT_TRUE(std::filesystem::create_directory(directory));}
  void TearDown()override{std::error_code e;if(std::filesystem::is_symlink(directory,e)||e)return;const auto resolved=std::filesystem::weakly_canonical(directory,e);if(e)return;const auto parent=std::filesystem::weakly_canonical(std::filesystem::temp_directory_path(),e);if(!e&&resolved.parent_path()==parent&&resolved.filename()==directory.filename()&&resolved.filename().string().starts_with("container-nano-primitives-test-"))std::filesystem::remove_all(resolved,e);}
  std::filesystem::path path(std::string name)const{return directory/name;}
  auto read(const std::filesystem::path& name){std::ifstream stream(name,std::ios::binary);return nv::io::readGrid<nv::HostBuffer>(stream,0,nv::HostBuffer{});}
  static double phi(vdb::NanoVdbPrimitiveShape shape,const vdb::NanoVdbPrimitiveOptions& o,int x,int y,int z){
    const double a=std::fma(o.voxelSize,double(x),o.origin[0])-o.center[0],b=std::fma(o.voxelSize,double(y),o.origin[1])-o.center[1],c=std::fma(o.voxelSize,double(z),o.origin[2])-o.center[2];
    if(shape==vdb::NanoVdbPrimitiveShape::Sphere)return std::hypot(a,b,c)-o.radius;
    if(shape==vdb::NanoVdbPrimitiveShape::Torus)return std::hypot(std::hypot(a,c)-o.majorRadius,b)-o.minorRadius;
    const double u=std::abs(a)-o.boxDimensions[0]*.5,v=std::abs(b)-o.boxDimensions[1]*.5,w=std::abs(c)-o.boxDimensions[2]*.5;
    return std::hypot(std::max(0.,u),std::max(0.,v),std::max(0.,w))+std::min(0.,std::max({u,v,w}));
  }
  template<class T>void analytic(const nv::NanoGrid<T>& grid,vdb::NanoVdbPrimitiveShape shape,const vdb::NanoVdbPrimitiveOptions& o){
    const auto acc=grid.getAccessor();const double band=o.halfWidth*o.voxelSize,tolerance=std::is_same_v<T,float>?2e-7:2e-14;uint64_t checked=0;
    for(int x=-14;x<=14;++x)for(int y=-14;y<=14;++y)for(int z=-14;z<=14;++z){const double sdf=phi(shape,o,x,y,z);const double expected=o.gridClass==vdb::NanoVdbPrimitiveClass::LevelSet?std::clamp(sdf,-band,band):std::clamp(-sdf/band,0.,1.);const double got=acc.getValue(nv::Coord(x,y,z));EXPECT_NEAR(got,expected,tolerance)<<x<<","<<y<<","<<z; ++checked;if(o.gridClass==vdb::NanoVdbPrimitiveClass::LevelSet&&std::abs(std::abs(sdf)-band)>1e-12)EXPECT_EQ(acc.isActive(nv::Coord(x,y,z)),std::abs(sdf)<band);}
    EXPECT_GT(checked,20000u);EXPECT_EQ(grid.gridName(),o.gridName);EXPECT_EQ(grid.gridClass(),o.gridClass==vdb::NanoVdbPrimitiveClass::LevelSet?nv::GridClass::LevelSet:nv::GridClass::FogVolume);
    if(o.gridClass==vdb::NanoVdbPrimitiveClass::Fog){EXPECT_EQ(grid.tree().root().background(),T(0));EXPECT_EQ(acc.getValue(nv::Coord(8192,8192,8192)),T(0));EXPECT_FALSE(acc.isActive(nv::Coord(8192,8192,8192)));}
  }
};
TEST_F(OpenVdbNanoPrimitives, AllThreeShapesBothClassesAndPrecisionsMatchIndependentWorldDistances) {
  int file=0;for(auto shape:{vdb::NanoVdbPrimitiveShape::Sphere,vdb::NanoVdbPrimitiveShape::Box,vdb::NanoVdbPrimitiveShape::Torus})for(auto cls:{vdb::NanoVdbPrimitiveClass::LevelSet,vdb::NanoVdbPrimitiveClass::Fog})for(auto storage:{vdb::NanoVdbPrimitiveStorage::Float,vdb::NanoVdbPrimitiveStorage::Double}){
    vdb::NanoVdbPrimitiveOptions o;o.gridClass=cls;o.storage=storage;o.center={.25,-.125,.375};o.origin={-.25,.125,-.125};o.gridName="analytic";const auto output=path(std::to_string(++file)+".nvdb");const auto r=vdb::WriteNanoVdbPrimitive(output,shape,o);ASSERT_EQ(r.file.grids.size(),1u);EXPECT_TRUE(r.file.grids[0].structureValidated);EXPECT_TRUE(r.file.grids[0].checksumVerified);EXPECT_LE(r.file.nativeBytes,r.nativeByteUpperBound);EXPECT_GT(r.workspaceUpperBound,0u);auto native=read(output);
    if(storage==vdb::NanoVdbPrimitiveStorage::Float){ASSERT_NE(native.grid<float>(),nullptr);analytic(*native.grid<float>(),shape,o);}else{ASSERT_NE(native.grid<double>(),nullptr);analytic(*native.grid<double>(),shape,o);EXPECT_EQ(r.roundedFloatValues,0u);EXPECT_EQ(r.maximumFloatAbsoluteError,0.);}
  }
}
TEST_F(OpenVdbNanoPrimitives, FloatErrorMetricsIncludeInactiveBackgroundAndAllPublishedValues) {
  vdb::NanoVdbPrimitiveOptions o;o.halfWidth=3.1;o.voxelSize=.125;const auto r=vdb::WriteNanoVdbPrimitive(path("float.nvdb"),vdb::NanoVdbPrimitiveShape::Sphere,o);o.storage=vdb::NanoVdbPrimitiveStorage::Double;(void)vdb::WriteNanoVdbPrimitive(path("double.nvdb"),vdb::NanoVdbPrimitiveShape::Sphere,o);auto a=read(path("float.nvdb")),b=read(path("double.nvdb"));const auto& f=*a.grid<float>();const auto& d=*b.grid<double>();EXPECT_GT(r.roundedFloatValues,0u);double error=std::abs(double(f.tree().root().background())-d.tree().root().background());EXPECT_GT(error,0.);const auto da=d.getAccessor();auto inspect=[&](const auto& node){for(auto it=node.cbeginValueAll();it;++it)error=std::max(error,std::abs(double(*it)-da.getValue(it.getCoord())));};inspect(f.tree().root());for(auto up=f.tree().root().cbeginChild();up;++up){inspect(*up);for(auto lo=up->cbeginChild();lo;++lo){inspect(*lo);for(auto leaf=lo->cbeginChild();leaf;++leaf)inspect(*leaf);}}EXPECT_EQ(r.maximumFloatAbsoluteError,error);
}
TEST_F(OpenVdbNanoPrimitives, MapCacheOptInIsExplicitBoundedAndDoubleMapRemainsNativeExact) {
  vdb::NanoVdbPrimitiveOptions o;o.voxelSize=.1;o.origin={.1,-.2,.3};const auto output=path("rounding.nvdb");EXPECT_THROW((void)vdb::WriteNanoVdbPrimitive(output,vdb::NanoVdbPrimitiveShape::Sphere,o),std::invalid_argument);EXPECT_FALSE(std::filesystem::exists(output));vdb::NanoVdbWriteOptions w;w.allowFloatMapRounding=true;const auto r=vdb::WriteNanoVdbPrimitive(output,vdb::NanoVdbPrimitiveShape::Sphere,o,w);EXPECT_TRUE(r.floatMapCachesRounded);EXPECT_GT(r.maximumFloatMapCacheErrorVoxels,0.);EXPECT_LE(r.maximumFloatMapCacheErrorVoxels,1e-4);auto n=read(output);for(int axis=0;axis<3;++axis){EXPECT_EQ(n.grid<float>()->map().mMatD[4*axis],.1);EXPECT_EQ(n.grid<float>()->map().mVecD[axis],o.origin[axis]);}
  o.origin={1000000000.125,0,0};o.center=o.origin;EXPECT_THROW((void)vdb::WriteNanoVdbPrimitive(path("lossy.nvdb"),vdb::NanoVdbPrimitiveShape::Sphere,o,w),std::invalid_argument);EXPECT_FALSE(std::filesystem::exists(path("lossy.nvdb")));
}
TEST_F(OpenVdbNanoPrimitives, DoubleGeometrySurvivesFloatPublicationWithoutEarlyParameterNarrowing) {
  vdb::NanoVdbPrimitiveOptions o;o.center={.25+1e-10,0,0};const auto r=vdb::WriteNanoVdbPrimitive(path("fine.nvdb"),vdb::NanoVdbPrimitiveShape::Sphere,o);auto n=read(path("fine.nvdb"));const double value=n.grid<float>()->tree().getValue(nv::Coord(10,0,0));EXPECT_LT(value,-5e-11);EXPECT_NEAR(value,-1e-10,3e-17);EXPECT_GT(r.roundedFloatValues,0u);
}
TEST_F(OpenVdbNanoPrimitives, NativeSignedContinuationAndFogSurfaceActivityAreExplicit) {
  const auto s=vdb::WriteNanoVdbPrimitive(path("sdf.nvdb"),vdb::NanoVdbPrimitiveShape::Sphere);auto native=read(path("sdf.nvdb"));const auto a=native.grid<float>()->getAccessor();EXPECT_EQ(a.getValue(nv::Coord(0)), -.375f);EXPECT_FALSE(a.isActive(nv::Coord(0)));EXPECT_EQ(a.getValue(nv::Coord(30,0,0)),.375f);EXPECT_FALSE(a.isActive(nv::Coord(30,0,0)));vdb::NanoVdbPrimitiveOptions o;o.gridClass=vdb::NanoVdbPrimitiveClass::Fog;(void)vdb::WriteNanoVdbPrimitive(path("fog.nvdb"),vdb::NanoVdbPrimitiveShape::Sphere,o);auto fog=read(path("fog.nvdb"));const auto b=fog.grid<float>()->getAccessor();EXPECT_EQ(b.getValue(nv::Coord(0)),1.f);EXPECT_TRUE(b.isActive(nv::Coord(0)));EXPECT_EQ(b.getValue(nv::Coord(8,0,0)),0.f);EXPECT_TRUE(b.isActive(nv::Coord(8,0,0)));EXPECT_EQ(b.getValue(nv::Coord(30,0,0)),0.f);EXPECT_FALSE(b.isActive(nv::Coord(30,0,0)));EXPECT_GT(s.file.grids[0].activeVoxels,0u);
}
TEST_F(OpenVdbNanoPrimitives, NativeStatisticsModesAndChecksumModesAreHonored) {
  int i=0;for(auto stats:{vdb::NanoVdbStatisticsMode::Bounds,vdb::NanoVdbStatisticsMode::MinMax,vdb::NanoVdbStatisticsMode::All})for(auto checksum:{vdb::NanoVdbChecksum::Disabled,vdb::NanoVdbChecksum::Header,vdb::NanoVdbChecksum::Full}){vdb::NanoVdbPrimitiveOptions o;o.statistics=stats;vdb::NanoVdbWriteOptions w;w.checksum=checksum;const auto r=vdb::WriteNanoVdbPrimitive(path(std::to_string(i++)+".nvdb"),vdb::NanoVdbPrimitiveShape::Sphere,o,w);EXPECT_EQ(r.file.grids[0].checksum,checksum);auto n=read(path(std::to_string(i-1)+".nvdb"));const auto* g=n.grid<float>();EXPECT_TRUE(g->hasBBox());EXPECT_EQ(g->hasMinMax(),stats!=vdb::NanoVdbStatisticsMode::Bounds);EXPECT_EQ(g->hasAverage(),stats==vdb::NanoVdbStatisticsMode::All);EXPECT_EQ(g->hasStdDeviation(),stats==vdb::NanoVdbStatisticsMode::All);}
}
TEST_F(OpenVdbNanoPrimitives, FogCachesMatchIndependentActiveVoxelMoments) {
  vdb::NanoVdbPrimitiveOptions o;o.gridClass=vdb::NanoVdbPrimitiveClass::Fog;o.storage=vdb::NanoVdbPrimitiveStorage::Double;(void)vdb::WriteNanoVdbPrimitive(path("moments.nvdb"),vdb::NanoVdbPrimitiveShape::Sphere,o);auto n=read(path("moments.nvdb"));const auto& g=*n.grid<double>();const auto a=g.getAccessor();const auto box=g.indexBBox();long double sum=0,squares=0;uint64_t count=0;double low=1,high=0;for(int x=box.min()[0];x<=box.max()[0];++x)for(int y=box.min()[1];y<=box.max()[1];++y)for(int z=box.min()[2];z<=box.max()[2];++z){const nv::Coord p(x,y,z);if(!a.isActive(p))continue;const double value=a.getValue(p);sum+=value;squares+=static_cast<long double>(value)*value;low=std::min(low,value);high=std::max(high,value);++count;}ASSERT_GT(count,0u);const double mean=double(sum/count),sd=double(std::sqrt(squares/count-(sum/count)*(sum/count)));EXPECT_EQ(g.activeVoxelCount(),count);EXPECT_EQ(g.tree().root().minimum(),low);EXPECT_EQ(g.tree().root().maximum(),high);EXPECT_NEAR(g.tree().root().average(),mean,2e-14);EXPECT_NEAR(g.tree().root().stdDeviation(),sd,2e-14);
}
TEST_F(OpenVdbNanoPrimitives, TinyAndLargePhysicalVoxelUnitsStayNative) {
  vdb::NanoVdbPrimitiveOptions o;o.voxelSize=1e-6;o.radius=4e-6;o.storage=vdb::NanoVdbPrimitiveStorage::Double;vdb::NanoVdbWriteOptions w;w.allowFloatMapRounding=true;(void)vdb::WriteNanoVdbPrimitive(path("tiny.nvdb"),vdb::NanoVdbPrimitiveShape::Sphere,o,w);auto tiny=read(path("tiny.nvdb"));EXPECT_NEAR(tiny.grid<double>()->tree().getValue(nv::Coord(5,0,0)),1e-6,2e-21);EXPECT_EQ(tiny.grid<double>()->map().mMatD[0],1e-6);o.voxelSize=262144;o.radius=524288;w.allowFloatMapRounding=false;(void)vdb::WriteNanoVdbPrimitive(path("large.nvdb"),vdb::NanoVdbPrimitiveShape::Sphere,o,w);auto large=read(path("large.nvdb"));EXPECT_EQ(large.grid<double>()->tree().getValue(nv::Coord(3,0,0)),262144.);EXPECT_EQ(large.grid<double>()->map().mMatD[0],262144.);
}
TEST_F(OpenVdbNanoPrimitives, RawAndAvailableCompressedWritersReadBackIdenticalValues) {
  const auto cap=vdb::NanoVdbInterchangeCapabilities();int i=0;for(auto codec:{vdb::NanoVdbCodec::None,vdb::NanoVdbCodec::Zip,vdb::NanoVdbCodec::Blosc}){if((codec==vdb::NanoVdbCodec::Zip&&!cap.zip)||(codec==vdb::NanoVdbCodec::Blosc&&!cap.blosc))continue;vdb::NanoVdbWriteOptions w;w.codec=codec;const auto r=vdb::WriteNanoVdbPrimitive(path(std::to_string(i++)+".nvdb"),vdb::NanoVdbPrimitiveShape::Box,{},w);EXPECT_EQ(r.file.grids[0].codec,codec);auto n=read(path(std::to_string(i-1)+".nvdb"));EXPECT_EQ(n.grid<float>()->tree().getValue(nv::Coord(8,0,0)),0.f);}
  vdb::NanoVdbWriteOptions w;w.raw=true;const auto r=vdb::WriteNanoVdbPrimitive(path("raw.nvdb"),vdb::NanoVdbPrimitiveShape::Sphere,{},w);EXPECT_TRUE(r.file.raw);auto n=read(path("raw.nvdb"));EXPECT_EQ(n.grid<float>()->tree().getValue(nv::Coord(8,0,0)),0.f);
}
TEST_F(OpenVdbNanoPrimitives, UnicodeParentNameAndExclusiveOutputPreserveExistingBytes) {
  const auto parent=directory/std::filesystem::path(L"\u03bb-\u6d4b\u8bd5");ASSERT_TRUE(std::filesystem::create_directory(parent));const auto output=parent/L"\u7403.nvdb";vdb::NanoVdbPrimitiveOptions o;o.gridName="\xce\xbb-\xe7\x90\x83";(void)vdb::WriteNanoVdbPrimitive(output,vdb::NanoVdbPrimitiveShape::Sphere,o);std::ifstream in(output,std::ios::binary);const std::string original((std::istreambuf_iterator<char>(in)),{});EXPECT_THROW((void)vdb::WriteNanoVdbPrimitive(output,vdb::NanoVdbPrimitiveShape::Sphere,o),std::invalid_argument);std::ifstream after(output,std::ios::binary);EXPECT_EQ(std::string((std::istreambuf_iterator<char>(after)),{}),original);EXPECT_EQ(vdb::InspectNanoVdbFile(output).grids[0].name,o.gridName);
}
TEST_F(OpenVdbNanoPrimitives, ShapeRelevanceFiniteCoordinatesAndResolvedSupportAreStrict) {
  auto reject=[&](vdb::NanoVdbPrimitiveOptions o,vdb::NanoVdbPrimitiveShape shape){const auto p=path("bad.nvdb");EXPECT_THROW((void)vdb::WriteNanoVdbPrimitive(p,shape,o),std::invalid_argument);EXPECT_FALSE(std::filesystem::exists(p));};vdb::NanoVdbPrimitiveOptions o;o.radius=.1;reject(o,vdb::NanoVdbPrimitiveShape::Sphere);reject(o,vdb::NanoVdbPrimitiveShape::Box);o={};o.boxDimensions[0]=.1;reject(o,vdb::NanoVdbPrimitiveShape::Box);reject(o,vdb::NanoVdbPrimitiveShape::Sphere);o={};o.majorRadius=.2;reject(o,vdb::NanoVdbPrimitiveShape::Torus);o={};o.center[0]=NAN;reject(o,vdb::NanoVdbPrimitiveShape::Sphere);o={};o.gridName=std::string("\xff",1);reject(o,vdb::NanoVdbPrimitiveShape::Sphere);o={};o.center[0]=1e12;reject(o,vdb::NanoVdbPrimitiveShape::Sphere);o={};o.radius=1e6;reject(o,vdb::NanoVdbPrimitiveShape::Sphere);
}
TEST_F(OpenVdbNanoPrimitives, SharedCapsRejectBeforeNativeBuildAndLeaveNoOutput) {
  const auto baseline=vdb::WriteNanoVdbPrimitive(path("reference.nvdb"),vdb::NanoVdbPrimitiveShape::Sphere);std::vector<vdb::NanoVdbLimits> limits(9);limits[0].maxActiveVoxels=baseline.candidateVoxels-1;limits[1].maxStoredLeafVoxels=baseline.storedLeafVoxelUpperBound-1;limits[2].maxNodes=baseline.nodeUpperBound-1;limits[3].maxNativeBytes=baseline.nativeByteUpperBound-1;limits[4].maxInputBytes=baseline.nativeByteUpperBound;limits[5].maxWorkspaceBytes=baseline.workspaceUpperBound-1;limits[6].maxWork=baseline.workUpperBound-1;limits[7].maxNameBytes=2;limits[8].maxMetadataBytes=0;for(size_t i=0;i<limits.size();++i){const auto p=path("cap"+std::to_string(i)+".nvdb");EXPECT_THROW((void)vdb::WriteNanoVdbPrimitive(p,vdb::NanoVdbPrimitiveShape::Sphere,{},{},limits[i]),std::exception)<<i;EXPECT_FALSE(std::filesystem::exists(p));}
}
TEST_F(OpenVdbNanoPrimitives, ReportedJointWorkspaceBoundAlsoCompletesCheckedIoPipeline) {
  const auto baseline=vdb::WriteNanoVdbPrimitive(path("first.nvdb"),vdb::NanoVdbPrimitiveShape::Sphere);vdb::NanoVdbLimits limits;limits.maxWorkspaceBytes=baseline.workspaceUpperBound;limits.maxWork=baseline.workUpperBound;const auto bounded=vdb::WriteNanoVdbPrimitive(path("bounded.nvdb"),vdb::NanoVdbPrimitiveShape::Sphere,{},{},limits);EXPECT_EQ(bounded.workspaceUpperBound,baseline.workspaceUpperBound);EXPECT_TRUE(bounded.file.grids[0].checksumVerified);
}
#endif
} // namespace
