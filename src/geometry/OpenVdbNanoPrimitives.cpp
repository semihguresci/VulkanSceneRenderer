#include "Container/geometry/OpenVdbNanoPrimitives.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <thread>
#include <type_traits>
#if defined(CONTAINER_HAS_NANOVDB_IO) && CONTAINER_HAS_NANOVDB_IO
#include "Container/geometry/detail/OpenVdbNanoCompatibility.h"
#include <nanovdb/tools/CreatePrimitives.h>
#if defined(NANOVDB_USE_TBB)
#include <tbb/task_arena.h>
#endif
#endif
namespace container::geometry::vdb {
namespace {
[[noreturn]] void unavailable(){throw std::runtime_error("Native NanoVDB primitive authoring is unavailable; enable ENABLE_NANOVDB_IO.");}
#if defined(CONTAINER_HAS_NANOVDB_IO) && CONTAINER_HAS_NANOVDB_IO
namespace nv=nanovdb;using Build=nv::tools::build::Grid<double>;using C=nv::Coord;using V=nv::Vec3d;
constexpr uint64_t Fixed=8ull*1024*1024;
void require(bool b,const char* s){if(!b)throw std::invalid_argument(s);}
uint64_t add(uint64_t a,uint64_t b){require(b<=UINT64_MAX-a,"NanoVDB primitive bound addition overflows");return a+b;}
uint64_t mul(uint64_t a,uint64_t b){require(!a||b<=UINT64_MAX/a,"NanoVDB primitive bound product overflows");return a*b;}
double gap(double x){return std::max(std::nextafter(x,INFINITY)-x,x-std::nextafter(x,-INFINITY));}
bool utf8(std::string_view s){for(size_t i=0;i<s.size();){const auto a=static_cast<unsigned char>(s[i++]);if(a<0x80)continue;size_t n;unsigned char lo=0x80,hi=0xbf;if(a>=0xc2&&a<=0xdf)n=1;else if(a>=0xe0&&a<=0xef){n=2;if(a==0xe0)lo=0xa0;if(a==0xed)hi=0x9f;}else if(a>=0xf0&&a<=0xf4){n=3;if(a==0xf0)lo=0x90;if(a==0xf4)hi=0x8f;}else return false;if(n>s.size()-i)return false;const auto b=static_cast<unsigned char>(s[i++]);if(b<lo||b>hi)return false;while(--n){const auto c=static_cast<unsigned char>(s[i++]);if(c<0x80||c>0xbf)return false;}}return true;}
void validate(const NanoVdbPrimitiveOptions& o,NanoVdbPrimitiveShape shape,const NanoVdbWriteOptions& w,const NanoVdbLimits& l){
  require(l.maxInputBytes&&l.maxInputBytes<=(1ull<<30)&&l.maxNativeBytes&&l.maxNativeBytes<=(1ull<<30)&&l.maxWorkspaceBytes&&l.maxWorkspaceBytes<=(16ull<<30)&&l.maxGrids&&l.maxGrids<=256&&l.maxNameBytes&&l.maxNameBytes<=65536&&l.maxMetadataBytes&&l.maxMetadataBytes<=(16u<<20)&&l.maxNodes&&l.maxNodes<=1'000'000&&l.maxWork&&l.maxWork<=4'000'000'000ull&&l.maxActiveVoxels&&l.maxStoredLeafVoxels,"NanoVDB primitive limits are invalid");
  require(shape>=NanoVdbPrimitiveShape::Sphere&&shape<=NanoVdbPrimitiveShape::Torus&&o.storage>=NanoVdbPrimitiveStorage::Float&&o.storage<=NanoVdbPrimitiveStorage::Double&&o.gridClass>=NanoVdbPrimitiveClass::LevelSet&&o.gridClass<=NanoVdbPrimitiveClass::Fog&&o.statistics>=NanoVdbStatisticsMode::Bounds&&o.statistics<=NanoVdbStatisticsMode::All,"NanoVDB primitive shape/storage/class/statistics option is invalid");
  require(std::isfinite(o.voxelSize)&&o.voxelSize>=1e-6&&o.voxelSize<=1e6&&std::isfinite(o.halfWidth)&&o.halfWidth>=1&&o.halfWidth<=64,"NanoVDB primitive h/band bounds are invalid");
  for(double n:{o.radius,o.majorRadius,o.minorRadius,o.boxDimensions[0],o.boxDimensions[1],o.boxDimensions[2]})require(std::isfinite(n)&&n>=1e-6&&n<=1e6,"NanoVDB primitive dimension/radius bounds are invalid");
  for(int a=0;a<3;++a)require(std::isfinite(o.center[a])&&std::abs(o.center[a])<=1e12&&std::isfinite(o.origin[a])&&std::abs(o.origin[a])<=1e12,"NanoVDB primitive center/origin bounds are invalid");
  require(shape==NanoVdbPrimitiveShape::Sphere||(o.radius==1),"unused sphere radius must retain its default");
  require(shape==NanoVdbPrimitiveShape::Box||o.boxDimensions==std::array<double,3>{2,2,2},"unused box dimensions must retain their defaults");
  require(shape==NanoVdbPrimitiveShape::Torus||(o.majorRadius==1&&o.minorRadius==.25),"unused torus radii must retain their defaults");
  if(shape==NanoVdbPrimitiveShape::Sphere)require(o.radius/o.voxelSize>=1.5,"sub-Nyquist sphere would be silently empty in native tools");
  if(shape==NanoVdbPrimitiveShape::Box)for(double n:o.boxDimensions)require(n/(2*o.voxelSize)>=1.5,"sub-Nyquist box would be silently empty in native tools");
  if(shape==NanoVdbPrimitiveShape::Torus)require(o.majorRadius>o.minorRadius&&o.minorRadius/o.voxelSize>=1.5,"native torus requires major>minor and a resolved minor radius");
  require(!o.gridName.empty()&&o.gridName.size()<=std::min<uint32_t>(256,l.maxNameBytes)&&o.gridName.find('\0')==std::string::npos&&utf8(o.gridName),"NanoVDB primitive name requires1..256 valid UTF-8 bytes without NUL");
  require(w.codec>=NanoVdbCodec::None&&w.codec<=NanoVdbCodec::Blosc&&w.checksum>=NanoVdbChecksum::Disabled&&w.checksum<=NanoVdbChecksum::Full,"NanoVDB primitive writer codec/checksum is invalid");
  require(!w.raw||w.codec==NanoVdbCodec::None,"raw NanoVDB cannot use a compressed codec");const auto capabilities=NanoVdbInterchangeCapabilities();
  require(w.codec!=NanoVdbCodec::Zip||capabilities.zip,"NanoVDB ZIP codec is unavailable");require(w.codec!=NanoVdbCodec::Blosc||capabilities.blosc,"NanoVDB BLOSC codec is unavailable");
}
int64_t block(int64_t x,int64_t width){int64_t q=x/width;if(x%width<0)--q;return q;}
struct Plan {
  NanoVdbPrimitiveResult report;nv::Map map;uint64_t leaf{},lower{},upper{},slots{},authorWork{},persistent{};
};
template<class T>uint64_t nativeBytes(uint64_t leaf,uint64_t lower,uint64_t upper){return add(8192,add(sizeof(nv::NanoGrid<T>)+sizeof(nv::NanoTree<T>),add(nv::NanoRoot<T>::memUsage(uint32_t(upper)),add(mul(leaf,sizeof(typename nv::NanoLeaf<T>::DataType)),add(mul(lower,nv::NanoLower<T>::memUsage()),mul(upper,nv::NanoUpper<T>::memUsage()))))));}
Plan plan(const NanoVdbPrimitiveOptions& o,NanoVdbPrimitiveShape shape,const NanoVdbWriteOptions& w,const NanoVdbLimits& l){
  Plan p;p.map.set(o.voxelSize,V(o.origin[0],o.origin[1],o.origin[2]));p.leaf=p.lower=p.upper=1;p.report.candidateVoxels=1;
  for(int a=0;a<3;++a){const double delta=o.center[a]-o.origin[a],center=delta/o.voxelSize;
    require(std::isfinite(center)&&std::abs(std::fma(-center,o.voxelSize,delta))/o.voxelSize+gap(center)<=1e-4,"NanoVDB primitive center index loses fractional precision");
    const double radius=shape==NanoVdbPrimitiveShape::Sphere?o.radius:shape==NanoVdbPrimitiveShape::Box?.5*o.boxDimensions[a]:(a==1?o.minorRadius:o.majorRadius+o.minorRadius);
    const double extent=radius/o.voxelSize+o.halfWidth;
    const double minimum=std::floor(center-extent)-1,maximum=std::ceil(center+extent)+1;
    require(std::isfinite(minimum)&&std::isfinite(maximum)&&minimum>=double(INT32_MIN)+8192&&maximum<=double(INT32_MAX)-8192,"NanoVDB primitive candidate bounds exceed safe Int32 node margins");
    p.report.candidateMinimum[a]=int32_t(minimum);p.report.candidateMaximum[a]=int32_t(maximum);
    const int64_t lo=int64_t(minimum),hi=int64_t(maximum);p.report.candidateVoxels=mul(p.report.candidateVoxels,uint64_t(hi-lo+1));
    p.leaf=mul(p.leaf,uint64_t(block(hi,8)-block(lo,8)+1));p.lower=mul(p.lower,uint64_t(block(hi,128)-block(lo,128)+1));p.upper=mul(p.upper,uint64_t(block(hi,4096)-block(lo,4096)+1));
    const bool rounded=double(p.map.mMatF[4*a])!=p.map.mMatD[4*a]||double(p.map.mInvMatF[4*a])!=p.map.mInvMatD[4*a]||double(p.map.mVecF[a])!=p.map.mVecD[a];p.report.floatMapCachesRounded|=rounded;
    for(double q:{minimum,maximum}){const double world=std::fma(o.voxelSize,q,o.origin[a]);require(std::isfinite(world)&&2*gap(world)<=o.voxelSize*1e-4,"NanoVDB primitive WORLD positions lose voxel precision");
      const double forward=std::abs(std::fma(double(p.map.mMatF[4*a])-p.map.mMatD[4*a],q,double(p.map.mVecF[a])-p.map.mVecD[a]))/o.voxelSize;
      const double inverse=std::abs(std::fma((double(p.map.mInvMatF[4*a])-p.map.mInvMatD[4*a])*o.voxelSize,q,double(p.map.mInvMatF[4*a])*(p.map.mVecD[a]-double(p.map.mVecF[a]))));
      p.report.maximumFloatMapCacheErrorVoxels=std::max({p.report.maximumFloatMapCacheErrorVoxels,forward,inverse});
    }
  }
  require(!p.report.floatMapCachesRounded||w.allowFloatMapRounding,"native Float map caches require explicit allowFloatMapRounding");
  require(std::isfinite(p.report.maximumFloatMapCacheErrorVoxels)&&p.report.maximumFloatMapCacheErrorVoxels<=1e-4,"native Float map cache discrepancy exceeds1e-4 voxel across candidate domain");
  p.report.storedLeafVoxelUpperBound=mul(p.leaf,512);p.report.nodeUpperBound=add(1,add(p.leaf,add(p.lower,p.upper)));
  require(p.report.candidateVoxels<=l.maxActiveVoxels&&p.report.storedLeafVoxelUpperBound<=l.maxStoredLeafVoxels&&p.report.nodeUpperBound<=l.maxNodes,"NanoVDB primitive full candidate/leaf/node preflight exceeds caps before allocation");
  p.report.nativeByteUpperBound=o.storage==NanoVdbPrimitiveStorage::Float?nativeBytes<float>(p.leaf,p.lower,p.upper):nativeBytes<double>(p.leaf,p.lower,p.upper);
  const uint64_t encoded=add(p.report.nativeByteUpperBound,w.codec==NanoVdbCodec::None?8192:add(p.report.nativeByteUpperBound/512,65536));
  require(p.report.nativeByteUpperBound<=l.maxNativeBytes&&encoded<=l.maxInputBytes,"NanoVDB primitive native/encoded bounds exceed caps before allocation");
  const uint64_t builder=add(sizeof(Build),add(mul(p.upper,sizeof(Build::Node2)+384),add(mul(p.lower,sizeof(Build::Node1)+128),mul(p.leaf,sizeof(Build::Node0)+128))));
  p.slots=add(p.upper,add(mul(p.upper,32768),add(mul(p.lower,4096),p.report.storedLeafVoxelUpperBound)));
  const uint64_t workers=
#if defined(NANOVDB_USE_TBB)
    1;
#else
    std::max(1u,std::thread::hardware_concurrency()/2);
#endif
  require(workers<=1024,"native CPU worker backend count exceeds bounded primitive policy");p.persistent=add(Fixed,mul(o.gridName.size(),32));
  p.report.workspaceUpperBound=add(p.persistent,add(Fixed,add(mul(builder,8),add(mul(p.report.nativeByteUpperBound,12),add(mul(p.report.nodeUpperBound,256),mul(workers,65536))))));
  require(p.report.workspaceUpperBound<=l.maxWorkspaceBytes,"NanoVDB primitive joint builder/native/intermediate/writer workspace exceeds cap before allocation");
  p.authorWork=add(mul(p.report.candidateVoxels,128),add(mul(p.slots,64),mul(p.report.nativeByteUpperBound,4)));
  p.report.workUpperBound=add(Fixed,add(p.authorWork,add(mul(p.slots,64),mul(p.report.nativeByteUpperBound,12))));
  require(p.report.workUpperBound<=l.maxWork,"NanoVDB primitive total build/flood/conversion/audit/statistics/CRC work exceeds cap before allocation");return p;
}
struct Temporary {std::filesystem::path directory,raw;
  Temporary(){static std::atomic_uint64_t serial{};for(int n=0;n<64;++n){auto candidate=std::filesystem::temp_directory_path()/("container-nano-primitives-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+"-"+std::to_string(++serial));if(std::filesystem::create_directory(candidate)){directory=std::move(candidate);raw=directory/"primitive.nvdb";return;}}throw std::runtime_error("cannot reserve native primitive temporary directory");}
  ~Temporary(){std::error_code e;std::filesystem::remove(raw,e);std::filesystem::remove(directory,e);}
};
template<class T>void audit(nv::NanoGrid<T>& grid,Build& source,const NanoVdbPrimitiveOptions& o,Plan& p,const NanoVdbLimits& l){
  const auto& tree=grid.tree();auto acc=source.getAccessor();
  require(tree.nodeCount(0)<=p.leaf&&tree.nodeCount(1)<=p.lower&&tree.nodeCount(2)<=p.upper&&grid.activeVoxelCount()<=p.report.candidateVoxels,"native primitive exceeded its preflight topology");
  require(grid.activeVoxelCount()>0,"native primitive unexpectedly has no active voxels");
  const auto compare=[&](double original,T published){require(std::isfinite(original)&&std::isfinite(double(published))&&!(original!=0&&published==T(0)),"native primitive Float/Double value conversion overflows/underflows");if constexpr(std::is_same_v<T,float>)if(double(published)!=original){++p.report.roundedFloatValues;p.report.maximumFloatAbsoluteError=std::max(p.report.maximumFloatAbsoluteError,std::abs(double(published)-original));}};
  compare(source.root().mBackground,tree.root().background());
  auto values=[&](const auto& node){for(auto v=node.cbeginValueAll();v;++v)compare(acc.getValue(v.getCoord()),*v);};
  const auto& root=tree.root();values(root);for(auto upper=root.cbeginChild();upper;++upper){values(*upper);for(auto lower=upper->cbeginChild();lower;++lower){values(*lower);for(auto leaf=lower->cbeginChild();leaf;++leaf)values(*leaf);}}
  const auto bounds=grid.indexBBox();for(int a=0;a<3;++a)require(bounds.min()[a]>=p.report.candidateMinimum[a]&&bounds.max()[a]<=p.report.candidateMaximum[a],"native primitive active bounds exceed the proved candidate domain");
  const auto& map=grid.map();for(int a=0;a<9;++a)require(map.mMatD[a]==p.map.mMatD[a]&&map.mInvMatD[a]==p.map.mInvMatD[a]&&map.mMatF[a]==p.map.mMatF[a]&&map.mInvMatF[a]==p.map.mInvMatF[a],"native primitive serialization changed map coefficients");for(int a=0;a<3;++a)require(map.mVecD[a]==o.origin[a]&&map.mVecF[a]==p.map.mVecF[a],"native primitive serialization changed origin coefficients");
  require(grid.gridName()==o.gridName,"native primitive serialization changed grid name");(void)l;
}
nv::GridHandle<nv::HostBuffer> build(const NanoVdbPrimitiveOptions& o,NanoVdbPrimitiveShape shape,Plan& p,const NanoVdbLimits& l){
  const V center(o.center[0],o.center[1],o.center[2]),origin(o.origin[0],o.origin[1],o.origin[2]);std::shared_ptr<Build> source;
  if(shape==NanoVdbPrimitiveShape::Sphere)source=nv::tools::initSphere<double>(o.radius,center,o.voxelSize,o.halfWidth,origin);
  else if(shape==NanoVdbPrimitiveShape::Box)source=nv::tools::initBox<double>(o.boxDimensions[0],o.boxDimensions[1],o.boxDimensions[2],center,o.voxelSize,o.halfWidth,origin);
  else source=nv::tools::initTorus<double>(o.majorRadius,o.minorRadius,center,o.voxelSize,o.halfWidth,origin);
  source->setName(o.gridName);
  {nv::tools::build::NodeManager<Build> manager(*source);nv::tools::build::sdfToLevelSet(manager);if(o.gridClass==NanoVdbPrimitiveClass::Fog){nv::tools::build::levelSetToFog(manager,false);source->root().mBackground=0.;}}
  nv::tools::CreateNanoGrid<Build> converter(*source);converter.setStats(o.statistics==NanoVdbStatisticsMode::Bounds?nv::tools::StatsMode::BBox:o.statistics==NanoVdbStatisticsMode::MinMax?nv::tools::StatsMode::MinMax:nv::tools::StatsMode::All);converter.setChecksum(nv::CheckMode::Disable);
  auto handle=o.storage==NanoVdbPrimitiveStorage::Float?converter.getHandle<float>():converter.getHandle<double>();
  require(handle.size()<=p.report.nativeByteUpperBound,"native primitive buffer exceeded its preflight upper bound");
  if(o.storage==NanoVdbPrimitiveStorage::Float){auto* grid=handle.grid<float>();require(grid!=nullptr,"native Float primitive serialization failed");audit(*grid,*source,o,p,l);}else{auto* grid=handle.grid<double>();require(grid!=nullptr,"native Double primitive serialization failed");audit(*grid,*source,o,p,l);}return handle;
}
#endif
} // namespace
NanoVdbPrimitiveResult WriteNanoVdbPrimitive(const std::filesystem::path& output,NanoVdbPrimitiveShape shape,const NanoVdbPrimitiveOptions& o,const NanoVdbWriteOptions& w,const NanoVdbLimits& l){
#if defined(CONTAINER_HAS_NANOVDB_IO) && CONTAINER_HAS_NANOVDB_IO
  validate(o,shape,w,l);require(!output.empty()&&!std::filesystem::exists(output)&&std::filesystem::is_directory(output.has_parent_path()?output.parent_path():std::filesystem::path(".")),"NanoVDB primitive output must be new in an existing directory");auto p=plan(o,shape,w,l);Temporary temporary;
  auto execute=[&]()->NanoVdbPrimitiveResult {
  {auto handle=build(o,shape,p,l);
    std::ofstream stream(temporary.raw,std::ios::binary|std::ios::out|std::ios::noreplace);require(bool(stream),"cannot create native primitive raw staging file");stream.exceptions(std::ios::badbit|std::ios::failbit);stream.write(reinterpret_cast<const char*>(handle.data()),std::streamsize(handle.size()));stream.close();}
  auto pass=l;require(p.persistent<pass.maxWorkspaceBytes&&p.authorWork<pass.maxWork,"native primitive leaves no checked reader/writer budget");pass.maxWorkspaceBytes-=p.persistent;pass.maxInputBytes=p.report.nativeByteUpperBound;pass.maxNativeBytes=p.report.nativeByteUpperBound;pass.maxWork-=p.authorWork;
  // This generated archive has exactly one named grid and no private metadata
  // extension. Tighten the checked reader's *limit-sized* node/metadata scratch
  // reservations to the already proved topology, rather than accidentally
  // reserving all of an unrelated caller's generous maximum after the build.
  pass.maxNodes=p.report.nodeUpperBound;pass.maxMetadataBytes=1;pass.maxGrids=1;pass.maxNameBytes=uint32_t(o.gridName.size());
  auto checked=ReadNanoVdbFile(temporary.raw,{},pass);require(checked.info().work<pass.maxWork,"native primitive validation consumes remaining writer work");pass.maxWork-=checked.info().work;pass.maxInputBytes=l.maxInputBytes;
  p.report.diagnostics={"Native CPU Double primitive construction, signed flood fill and optional linear fog conversion precede checked Float/Double serialization.","Float conversion metrics cover the published background and all stored active/inactive voxel and tile values; geometric evaluation remains a native Double approximation.","The Double map is exact. Float map cache discrepancy reports stored coefficients across the candidate domain, excluding later Float-coordinate/arithmetic evaluation.","Native name/class/map/statistics/checksum are authored; arbitrary OpenVDB metadata, arbitrary transforms, CUDA and quantized backends are not provided.","Full leaf-aligned candidate, native builder/manager/thread scratch, intermediates, validation, codec, statistics/CRC and writer share logical caps; worker stacks/provider internals are not an RSS sandbox."};
  auto writer=w;writer.allowFloatMapRounding=false;static_assert(std::is_nothrow_move_assignable_v<NanoVdbFileInfo>);static_assert(std::is_nothrow_move_constructible_v<NanoVdbPrimitiveResult>);p.report.file=WriteNanoVdbFile(checked,output,writer,pass);return std::move(p.report);
  };
#if defined(NANOVDB_USE_TBB)
  return tbb::task_arena(1).execute(execute);
#else
  return execute();
#endif
#else
  (void)output;(void)shape;(void)o;(void)w;(void)l;unavailable();
#endif
}
} // namespace container::geometry::vdb
