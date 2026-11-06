#pragma once
#include "Container/geometry/OpenVdbFastSweeping.h"
#include "Container/geometry/OpenVdbGridSelection.h"
#include "OpenVdbFileUtils.h"
#include "OpenVdbProvenance.h"
#include <openvdb/tools/Statistics.h>
#include <openvdb/tools/Mask.h>
#include <tbb/task_arena.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <fstream>
#include <limits>
#include <set>

namespace container::geometry::vdb::fast_detail {
inline void add(uintmax_t &a, uintmax_t b, uintmax_t cap, const char *why) {
  if(a>cap || b>cap-a) throw std::invalid_argument(why); a+=b;
}
inline uintmax_t mul(uintmax_t a, uintmax_t b, uintmax_t cap, const char *why) {
  if(b && a>cap/b) throw std::invalid_argument(why); return a*b;
}
struct Budget {
  const ProcessingOptions &p; const FastSweepingOptions &o;
  uintmax_t encoded{}, resident{}, metadata{}, work{}, projected{}, slots{}, active{};
  void check(uintmax_t prospective=0) const {
    uintmax_t sum=4ull*1024*1024;
    const auto why="fast sweeping shared encoded/tree/growth/buffer/writer workspace exceeds maxInputBytes";
    add(sum,encoded,p.maxInputBytes,why);
    for(int n=0;n<16;++n) {
      add(sum,resident,p.maxInputBytes,why); add(sum,metadata,p.maxInputBytes,why);
      add(sum,projected,p.maxInputBytes,why); add(sum,prospective,p.maxInputBytes,why);
    }
  }
  void items(uintmax_t n) {add(work,n,o.maxWorkItems,"fast sweeping exceeds maxWorkItems");}
  void meta(const openvdb::MetaMap &m) {
    for(auto it=m.beginMeta();it!=m.endMeta();++it) {
      if(!it->second) throw std::invalid_argument("null fast sweeping metadata");
      add(metadata,it->first.size()+it->second->typeName().size()+256,p.maxInputBytes,"fast sweeping metadata exceeds byte cap");
      add(metadata,it->second->size(),p.maxInputBytes,"fast sweeping metadata exceeds byte cap");
    } check();
  }
};
struct Temporary {
  std::filesystem::path directory;
  Temporary() {
    static std::atomic_uint64_t sequence{};
    for(int n=0;n<64;++n) {
      auto p=std::filesystem::temp_directory_path()/("container-fast-sweeping-"+
          std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+"-"+std::to_string(++sequence));
      if(std::filesystem::create_directory(p)) {directory=std::move(p);return;}
    } throw std::runtime_error("cannot reserve fast sweeping snapshot directory");
  }
  ~Temporary() {
    std::error_code ignored;
    for(auto name:{"source.vdb","mask.vdb","field.vdb","result.vdb"}) std::filesystem::remove(directory/name,ignored);
    std::filesystem::remove(directory,ignored);
  }
};
inline void copy(const std::filesystem::path &from,const std::filesystem::path &to,uintmax_t cap) {
  std::ifstream source(from,std::ios::binary);
  std::ofstream dest(to,std::ios::binary|std::ios::out|std::ios::noreplace);
  const bool owned=dest.is_open();
  try {
    if(!source || !dest) throw std::runtime_error("cannot exclusively copy fast sweeping snapshot/output");
    uintmax_t total=0; std::array<char,64*1024> buffer{};
    while(source.read(buffer.data(),buffer.size()) || source.gcount()) {
      add(total,uintmax_t(source.gcount()),cap,"fast sweeping copy exceeds byte cap");
      dest.write(buffer.data(),source.gcount()); if(!dest) throw std::runtime_error("fast sweeping copy write failed");
    }
    if(!source.eof()) throw std::runtime_error("fast sweeping copy read failed");
    dest.flush(); dest.close(); if(!dest) throw std::runtime_error("fast sweeping copy flush failed");
  } catch(...) {
    dest.close(); if(owned) {std::error_code ignored;std::filesystem::remove(to,ignored);} throw;
  }
}
enum class Role { Scalar, Topology, Mask, Field };
inline bool supported(const openvdb::GridBase &g,Role role) {
  if(g.isType<openvdb::FloatGrid>() || g.isType<openvdb::DoubleGrid>()) return true;
  if(role==Role::Field) return g.isType<openvdb::Vec3SGrid>() || g.isType<openvdb::Vec3DGrid>();
  return role!=Role::Scalar && (g.isType<openvdb::BoolGrid>() || g.isType<openvdb::MaskGrid>() ||
      g.isType<openvdb::Int32Grid>() || g.isType<openvdb::Int64Grid>() || g.isType<openvdb::Vec3IGrid>() ||
      g.isType<openvdb::Vec3SGrid>() || g.isType<openvdb::Vec3DGrid>());
}
template<class Grid> uintmax_t fullMemory(const openvdb::GridBase &g) {
  return openvdb::tools::memUsageIfLoaded(static_cast<const Grid&>(g).tree(),false);
}
inline uintmax_t fullMemory(const openvdb::GridBase &g) {
  if(g.isType<openvdb::FloatGrid>()) return fullMemory<openvdb::FloatGrid>(g);
  if(g.isType<openvdb::DoubleGrid>()) return fullMemory<openvdb::DoubleGrid>(g);
  if(g.isType<openvdb::BoolGrid>()) return fullMemory<openvdb::BoolGrid>(g);
  if(g.isType<openvdb::MaskGrid>()) return fullMemory<openvdb::MaskGrid>(g);
  if(g.isType<openvdb::Vec3SGrid>()) return fullMemory<openvdb::Vec3SGrid>(g);
  if(g.isType<openvdb::Vec3DGrid>()) return fullMemory<openvdb::Vec3DGrid>(g);
  if(g.isType<openvdb::Int32Grid>()) return fullMemory<openvdb::Int32Grid>(g);
  if(g.isType<openvdb::Int64Grid>()) return fullMemory<openvdb::Int64Grid>(g);
  if(g.isType<openvdb::Vec3IGrid>()) return fullMemory<openvdb::Vec3IGrid>(g);
  throw std::logic_error("unsupported sweeping type");
}
inline openvdb::GridBase::Ptr load(const std::filesystem::path &path,const std::filesystem::path &snapshot,
    const std::string &selection,Role role,Budget &b,openvdb::MetaMap *fileMeta=nullptr) {
  if(path.empty() || !std::filesystem::is_regular_file(path)) throw std::invalid_argument("fast sweeping input must be a regular file");
  const auto size=std::filesystem::file_size(path);
  add(b.encoded,size,b.p.maxInputBytes,"aggregate sweeping encoded inputs exceed byte cap");b.check();
  copy(path,snapshot,size); if(std::filesystem::file_size(snapshot)!=size) throw std::runtime_error("fast sweeping input changed size");
  detail::RequireSeekableFile(snapshot);
  if(!openvdb::io::Archive::isDelayedLoadingEnabled())
    throw std::invalid_argument("bounded fast sweeping requires delayed loading; unset OPENVDB_DISABLE_DELAYED_LOAD and use a delayed-loading SDK");
  openvdb::io::File file(snapshot.string());file.setCopyMaxBytes(0);file.open(true);
  b.meta(*file.getMetadata()); if(fileMeta) *fileMeta=*file.getMetadata();
  std::set<std::string> names;
  for(auto it=file.beginName();it!=file.endName();++it) {
    const auto name=it.gridName();
    add(b.metadata,2*name.size()+128,b.p.maxInputBytes,"fast sweeping descriptor budget exceeded");b.check();
    if(names.size()>=4096 || !names.insert(name).second) throw std::invalid_argument("too many or colliding sweeping grid descriptors");
  }
  std::string chosen;bool found=false;
  for(auto it=file.beginName();it!=file.endName();++it) {
    const auto m=file.readGridMetadata(it.gridName());b.meta(*m);
    if(!GridSelectionMatches(selection,it.gridName(),m->getName())) continue;
    if(selection.empty() && !supported(*m,role)) continue;
    if(found) throw std::invalid_argument("ambiguous sweeping selector; use descriptor:<exact descriptor>");
    if(!supported(*m,role)) throw std::invalid_argument("unsupported fast sweeping grid type");
    const auto mem=m->getMetadata<openvdb::Int64Metadata>(openvdb::GridBase::META_FILE_MEM_BYTES);
    const auto count=m->getMetadata<openvdb::Int64Metadata>(openvdb::GridBase::META_FILE_VOXEL_COUNT);
    if(!mem || !count || mem->value()<0 || count->value()<0 || uintmax_t(count->value())>b.p.maxVoxelCount)
      throw std::invalid_argument("fast sweeping requires valid recorded tree/voxel statistics within budget");
    b.check(uintmax_t(mem->value()));chosen=it.gridName();found=true;if(selection.empty()) break;
  }
  if(!found) throw std::invalid_argument("no supported fast sweeping grid matches selection");
  auto grid=file.readGrid(chosen);const auto memory=fullMemory(*grid);
  add(b.resident,memory,b.p.maxInputBytes,"fast sweeping decoded inputs exceed byte cap");
  add(b.active,grid->activeVoxelCount(),b.p.maxVoxelCount,"aggregate sweeping input activity exceeds voxel cap");
  add(b.slots,mul(grid->baseTree().leafCount(),512,b.p.maxVoxelCount,"sweeping leaf slots exceed voxel cap"),b.p.maxVoxelCount,"aggregate sweeping leaf slots exceed voxel cap");
  b.items(grid->baseTree().leafCount()*512);b.check();grid->readNonresidentBuffers();
  if(grid->memUsage()>memory) throw std::runtime_error("sweeping decoded memory exceeded preflight");
  file.close();return grid;
}
inline double uniformMap(const openvdb::GridBase &g) {
  if(!g.transform().isLinear()) throw std::invalid_argument("fast sweeping requires uniform orthogonal affine voxels");
  const auto m=g.transform().baseMap();const auto origin=m->applyMap(openvdb::Vec3d(0));
  for(size_t i=0;i<3;++i) if(!std::isfinite(origin[i])) throw std::invalid_argument("nonfinite sweeping affine origin");
  std::array<openvdb::Vec3d,3> axis;double h=0;
  for(size_t i=0;i<3;++i) {
    openvdb::Vec3d e(0);e[i]=1;auto a=m->applyJacobian(e);auto inv=m->applyInverseJacobian(e);
    for(size_t j=0;j<3;++j) if(!std::isfinite(a[j]) || !std::isfinite(inv[j])) throw std::invalid_argument("invalid sweeping affine Jacobian");
    const auto length=std::hypot(a.x(),a.y(),a.z());if(!i) h=length;
    if(!std::isfinite(length) || !(length>0) || std::abs(length-h)>h*1e-12) throw std::invalid_argument("nonuniform sweeping voxels");
    axis[i]=a/length;
    for(size_t j=0;j<i;++j) if(std::abs(axis[i].dot(axis[j]))>1e-12) throw std::invalid_argument("fast sweeping rejects affine shear");
  }
  if(h<1e-6 || h>1e6) throw std::invalid_argument("sweeping voxel size must be in [1e-6,1e6]");return h;
}
inline void sameMap(const openvdb::GridBase &a,const openvdb::GridBase &b) {
  const auto fail=[] {throw std::invalid_argument("fast sweeping field/mask affine coefficients must match exactly");};
  if(!a.transform().isLinear() || !b.transform().isLinear()) fail();
  const auto x=a.transform().baseMap(),y=b.transform().baseMap();
  if(x->applyMap(openvdb::Vec3d(0))!=y->applyMap(openvdb::Vec3d(0))) fail();
  for(size_t i=0;i<3;++i) {openvdb::Vec3d e(0);e[i]=1;if(x->applyJacobian(e)!=y->applyJacobian(e)) fail();}
}
template<class Grid> openvdb::MaskGrid::Ptr maskTyped(const openvdb::GridBase &g) {
  return openvdb::MaskGrid::create(std::make_shared<openvdb::MaskTree>(static_cast<const Grid&>(g).tree(),false,openvdb::TopologyCopy()));
}
inline openvdb::MaskGrid::Ptr activityMask(const openvdb::GridBase &g) {
  openvdb::MaskGrid::Ptr m;
  if(g.isType<openvdb::FloatGrid>()) m=maskTyped<openvdb::FloatGrid>(g);
  else if(g.isType<openvdb::DoubleGrid>()) m=maskTyped<openvdb::DoubleGrid>(g);
  else if(g.isType<openvdb::BoolGrid>()) m=maskTyped<openvdb::BoolGrid>(g);
  else if(g.isType<openvdb::Int32Grid>()) m=maskTyped<openvdb::Int32Grid>(g);
  else if(g.isType<openvdb::Int64Grid>()) m=maskTyped<openvdb::Int64Grid>(g);
  else if(g.isType<openvdb::Vec3IGrid>()) m=maskTyped<openvdb::Vec3IGrid>(g);
  else if(g.isType<openvdb::Vec3SGrid>()) m=maskTyped<openvdb::Vec3SGrid>(g);
  else if(g.isType<openvdb::Vec3DGrid>()) m=maskTyped<openvdb::Vec3DGrid>(g);
  else m=maskTyped<openvdb::MaskGrid>(g);
  m->setTransform(openvdb::math::Transform::createLinearTransform(1));return m;
}
// Reserve a dense aligned tree of the entire finite prospective domain before
// SDK morphology/voxelization. This deliberately overestimates sparse layouts.
inline void domain(openvdb::CoordBBox box,uint32_t expansion,Budget &b) {
  if(box.empty()) throw std::invalid_argument("fast sweeping active domain is empty");
  uintmax_t cells=1,leaves=1,lower=1,upper=1;
  for(size_t i=0;i<3;++i) {
    const int64_t lo=int64_t(box.min()[i])-expansion-2,hi=int64_t(box.max()[i])+expansion+2;
    // Native slice hashes evaluate x+y+z in signed Coord component precision.
    if(lo<-(INT32_MAX/4) || hi>INT32_MAX/4) throw std::invalid_argument("sweeping stencil/slice coordinates require recentering");
    cells=mul(cells,uint64_t(hi-lo+1),b.p.maxVoxelCount,"prospective dense sweeping domain exceeds maxVoxelCount");
    const auto blocks=[&](int64_t d) {const auto floor=[&](int64_t n) {return n>=0?n/d:-((-n+d-1)/d);};return uint64_t(floor(hi)-floor(lo)+1);};
    leaves=mul(leaves,blocks(8),b.p.maxVoxelCount,"sweeping leaf count exceeds cap");
    lower=mul(lower,blocks(128),b.p.maxVoxelCount,"sweeping lower node count exceeds cap");
    upper=mul(upper,blocks(4096),b.p.maxVoxelCount,"sweeping upper node count exceeds cap");
  }
  const auto why="prospective dense sweeping tree exceeds maxInputBytes";
  uintmax_t bytes=0;
  add(bytes,mul(leaves,sizeof(openvdb::Vec3DTree::LeafNodeType)+512*sizeof(openvdb::Vec3d)+512,b.p.maxInputBytes,why),b.p.maxInputBytes,why);
  add(bytes,mul(lower,sizeof(openvdb::Vec3DTree::RootNodeType::ChildNodeType::ChildNodeType),b.p.maxInputBytes,why),b.p.maxInputBytes,why);
  add(bytes,mul(upper,sizeof(openvdb::Vec3DTree::RootNodeType::ChildNodeType)+256,b.p.maxInputBytes,why),b.p.maxInputBytes,why);
  b.projected=bytes;b.check();
  b.items(mul(cells,uint64_t(64)*(b.o.iterations+1),b.o.maxWorkItems,"sweeping iterations/domain exceed maxWorkItems"));
}
template<class V> inline bool finite(const V &v) {
  if constexpr(openvdb::VecTraits<V>::IsVec) return std::isfinite(v[0])&&std::isfinite(v[1])&&std::isfinite(v[2]);
  else return std::isfinite(v);
}
inline void semantics(const openvdb::GridBase &g) {
  const auto recognized=[&](const char *key,std::initializer_list<std::string> allowed) {
    const auto m=g[key];if(!m) return;
    const auto s=std::dynamic_pointer_cast<const openvdb::StringMetadata>(m);
    if(!s) throw std::invalid_argument("fast sweeping class/vector metadata must have native String type");
    auto text=s->value();openvdb::string::trim(text);openvdb::string::to_lower(text);
    if(std::find(allowed.begin(),allowed.end(),text)==allowed.end()) throw std::invalid_argument("unrecognized fast sweeping class/vector semantics");
  };
  recognized(openvdb::GridBase::META_GRID_CLASS,{openvdb::GridBase::gridClassToString(openvdb::GRID_UNKNOWN),openvdb::GridBase::gridClassToString(openvdb::GRID_LEVEL_SET),openvdb::GridBase::gridClassToString(openvdb::GRID_FOG_VOLUME),openvdb::GridBase::gridClassToString(openvdb::GRID_STAGGERED)});
  recognized(openvdb::GridBase::META_VECTOR_TYPE,{openvdb::GridBase::vecTypeToString(openvdb::VEC_INVARIANT),openvdb::GridBase::vecTypeToString(openvdb::VEC_COVARIANT),openvdb::GridBase::vecTypeToString(openvdb::VEC_COVARIANT_NORMALIZE),openvdb::GridBase::vecTypeToString(openvdb::VEC_CONTRAVARIANT_RELATIVE),openvdb::GridBase::vecTypeToString(openvdb::VEC_CONTRAVARIANT_ABSOLUTE)});
  const auto local=g[openvdb::GridBase::META_IS_LOCAL_SPACE];
  if(local && !std::dynamic_pointer_cast<const openvdb::BoolMetadata>(local)) throw std::invalid_argument("fast sweeping world/local metadata must have native Bool type");
}
template<class Grid> inline void values(const Grid &g,Budget &b,bool field=false) {
  semantics(g);
  if(!finite(g.background())) throw std::invalid_argument("nonfinite sweeping input background");
  for(auto it=g.cbeginValueAll();it;++it) {b.items(1);if(!finite(typename Grid::ValueType(*it))) throw std::invalid_argument("nonfinite stored sweeping input value");}
  if(field && (!g.isInWorldSpace() || g.getGridClass()==openvdb::GRID_LEVEL_SET || g.getGridClass()==openvdb::GRID_STAGGERED ||
      g.getVectorType()==openvdb::VEC_COVARIANT_NORMALIZE)) throw std::invalid_argument("extension requires an ordinary collocated world field");
  if(field) {
    if constexpr(openvdb::VecTraits<typename Grid::ValueType>::IsVec) {
      if(g.getGridClass()!=openvdb::GRID_UNKNOWN) throw std::invalid_argument("vector extension requires UNKNOWN class");
    } else if(g.getVectorType()!=openvdb::VEC_INVARIANT) throw std::invalid_argument("scalar extension requires invariant vector semantics");
  }
}
inline void clean(openvdb::GridBase &g,const std::string &name,const openvdb::GridBase &source) {
  detail::RemoveBoundaryProjectionMetadata(g);
  g.setTransform(detail::CopyTransformExact(source.transform()));g.setName(name);g.setSaveFloatAsHalf(false);
  for(const auto key:{openvdb::GridBase::META_FILE_BBOX_MIN,openvdb::GridBase::META_FILE_BBOX_MAX,openvdb::GridBase::META_FILE_MEM_BYTES,openvdb::GridBase::META_FILE_VOXEL_COUNT}) g.removeMeta(key);
  g.insertMeta("container:fast_sweeping",openvdb::StringMetadata("bounded native unit-index solve; world-distance output"));
}
struct NativeResult {openvdb::GridBase::Ptr distance,extension;uint64_t boundary{},sweeping{};};
NativeResult runFloat(const openvdb::GridBase &,const openvdb::GridBase *,const openvdb::MaskGrid *,FastSweepingOperation,bool,const FastSweepingOptions &,Budget &,double);
NativeResult runDouble(const openvdb::GridBase &,const openvdb::GridBase *,const openvdb::MaskGrid *,FastSweepingOperation,bool,const FastSweepingOptions &,Budget &,double);
} // namespace container::geometry::vdb::fast_detail
