#include "Container/geometry/OpenVdbNanoStatistics.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <type_traits>
#if defined(CONTAINER_HAS_NANOVDB_IO) && CONTAINER_HAS_NANOVDB_IO
#include <nanovdb/HostBuffer.h>
#include <nanovdb/tools/GridStats.h>
#include <nanovdb/tools/GridChecksum.h>
#include "OpenVdbNanoChecksum.h"
#if defined(NANOVDB_USE_TBB)
#include <tbb/task_arena.h>
#endif
#endif

namespace container::geometry::vdb {
namespace {
[[noreturn]] void unavailableStats() {
  throw std::runtime_error("Native NanoVDB statistics are unavailable; enable ENABLE_NANOVDB_IO.");
}
#if defined(CONTAINER_HAS_NANOVDB_IO) && CONTAINER_HAS_NANOVDB_IO
namespace nv=nanovdb;
constexpr uint64_t Fixed=4ull*1024*1024;
void check(bool value,const char* message) {
  if (!value) throw std::invalid_argument(message);
}
uint64_t plus(uint64_t a,uint64_t b) {
  check(b<=UINT64_MAX-a,"NanoVDB statistics count overflow"); return a+b;
}
uint64_t times(uint64_t a,uint64_t b) {
  check(!a || b<=UINT64_MAX/a,"NanoVDB statistics size overflow"); return a*b;
}
void charge(uint64_t& work,uint64_t n,const NanoVdbLimits& l) {
  work=plus(work,n); check(work<=l.maxWork,"NanoVDB statistics shared work budget exceeded");
}
struct PrivateFiles {
  std::filesystem::path directory,raw,updated;
  PrivateFiles() {
    static std::atomic_uint64_t serial{};
    for (int i=0;i<64;++i) {
      auto candidate=std::filesystem::temp_directory_path()/
          ("container-nano-statistics-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+
           "-"+std::to_string(++serial));
      if (std::filesystem::create_directory(candidate)) {
        directory=std::move(candidate); raw=directory/"input.nvdb"; updated=directory/"updated.nvdb"; return;
      }
    }
    throw std::runtime_error("cannot reserve private native statistics directory");
  }
  ~PrivateFiles() {
    std::error_code e;
    std::filesystem::remove(raw,e); std::filesystem::remove(updated,e);
    std::filesystem::remove(directory,e);
  }
};
template<class T> void finite(const T& value) {
  if constexpr (std::is_arithmetic_v<T>) {
    if constexpr (std::is_floating_point_v<T>)
      check(std::isfinite(value),"NanoVDB statistics produced a nonfinite cache");
  } else for (int k=0;k<3;++k) finite(value[k]);
}
template<class T> NanoVdbStatisticValue statistic(const T& value) {
  if constexpr (std::is_arithmetic_v<T>) return value;
  else return std::array<typename T::ValueType,3>{value[0],value[1],value[2]};
}
template<class T> void numericRange(const T& value,NanoVdbStatisticsMode mode,uint64_t active) {
  if (mode==NanoVdbStatisticsMode::Bounds) return;
  if constexpr (!std::is_arithmetic_v<T>) {
    const auto squared=value.lengthSqr();
    bool nonzero=false;
    for(int k=0;k<3;++k) nonzero|=value[k]!=0;
    check(std::isfinite(squared) && (!nonzero || squared>0),
          "native vector squared magnitude overflows/underflows; use bounds-only statistics or rescale values");
  }
  if (mode!=NanoVdbStatisticsMode::All) return;
  const double momentLimit=std::sqrt(std::numeric_limits<double>::max()/
      (64.0*std::max(1.0,double(active))));
  if constexpr (std::is_arithmetic_v<T>)
    check(std::abs(double(value))<=momentLimit,
          "native moment accumulation would overflow; use min/max-only statistics or rescale values");
  else check(double(value.lengthSqr())<=momentLimit,
          "native vector moment accumulation would overflow; use min/max-only statistics or rescale values");
}

template<class T>
void updateTyped(nv::HostBuffer& buffer,NanoVdbStatisticsMode requested,
                  NanoVdbStatisticsSummary& summary,uint64_t& work,const NanoVdbLimits& l) {
  auto& grid=*static_cast<nv::NanoGrid<T>*>(buffer.data());
  using Node2=nv::NanoUpper<T>;
  using Node1=nv::NanoLower<T>;
  auto& root=grid.tree().root();
  // Entirely inactive internal nodes can have an origin-erasing empty bbox
  // from upstream GridStats. Recover ownership before AND after recomputation.
  const auto restore=[&] {
    const auto walk=[&](auto&& self,auto& node,const nv::Coord& expected)->void {
      using Node=std::remove_reference_t<decltype(node)>;
      using Child=typename Node::ChildNodeType;
      auto* data=node.data();
      if (data->mBBox.empty()) {
        data->mBBox[0]=expected+nv::Coord(1); data->mBBox[1]=expected;
      }
      if constexpr (Child::LEVEL>0) {
        for(auto it=data->mChildMask.beginOn();it;++it) {
          const auto local=Node::OffsetToLocalCoord(*it);
          const nv::Coord origin(
            int32_t(int64_t(expected[0])+int64_t(local[0])*Child::DIM),
            int32_t(int64_t(expected[1])+int64_t(local[1])*Child::DIM),
            int32_t(int64_t(expected[2])+int64_t(local[2])*Child::DIM));
          self(self,*data->getChild(*it),origin);
        }
      }
    };
    auto* data=root.data();
    for(uint32_t i=0;i<data->mTableSize;++i) {
      auto* tile=data->tile(i);
      if(tile->isChild()) walk(walk,*data->getChild(tile),tile->origin());
    }
  };
  const auto& tree=*grid.tree().data();
  charge(work,plus(root.data()->mTableSize,
      plus(times(tree.mNodeCount[2],32768),plus(times(tree.mNodeCount[1],4096),times(tree.mNodeCount[0],512)))),l);
  restore();
  const auto effective=std::is_same_v<T,bool> || std::is_same_v<T,nv::ValueMask>
      ? NanoVdbStatisticsMode::Bounds : requested;
  const auto inspectValue=[&](const auto& it,uint32_t width) {
    const auto origin=it.getCoord();
    for(int k=0;k<3;++k)
      check(int64_t(origin[k])+width-1<INT32_MAX,
            "native statistics at the final Int32 voxel require recentering to avoid upstream exclusive-bound overflow");
    numericRange(*it,effective,summary.activeVoxels);
  };
  {
    for(auto it=root.cbeginValueOn();it;++it) inspectValue(it,Node2::DIM);
    for(auto it2=root.cbeginChild();it2;++it2) {
      for(auto values=it2->cbeginValueOn();values;++values) inspectValue(values,Node1::DIM);
      for(auto it1=it2->cbeginChild();it1;++it1) {
        for(auto values=it1->cbeginValueOn();values;++values) inspectValue(values,nv::NanoLeaf<T>::DIM);
        for(auto it0=it1->cbeginChild();it0;++it0)
          for(auto values=it0->cbeginValueOn();values;++values) inspectValue(values,1);
      }
    }
  }
  const auto mode=effective==NanoVdbStatisticsMode::Bounds ? nv::tools::StatsMode::BBox
      : effective==NanoVdbStatisticsMode::MinMax ? nv::tools::StatsMode::MinMax : nv::tools::StatsMode::All;
  nv::tools::updateGridStats(&grid,mode);
  restore();
  if(summary.activeVoxels) {
    const auto& bounds=root.bbox();
    check(!bounds.empty(),"native statistics lost nonempty occupancy bounds");
    summary.indexMinimum=std::array<int64_t,3>{bounds[0][0],bounds[0][1],bounds[0][2]};
    summary.indexMaximum=std::array<int64_t,3>{bounds[1][0],bounds[1][1],bounds[1][2]};
    // Upstream adds one in Int32 when building exclusive world bounds. Avoid
    // overflow for the valid final voxel at INT32_MAX.
    nv::Vec3d lo(std::numeric_limits<double>::infinity()),hi(-std::numeric_limits<double>::infinity());
    for(unsigned corner=0;corner<8;++corner) {
      nv::Vec3d index;
      for(int k=0;k<3;++k) index[k]=(corner&(1u<<k)) ? double(bounds[1][k])+1.0 : double(bounds[0][k]);
      const auto world=grid.map().applyMap(index);
      for(int k=0;k<3;++k) { lo[k]=std::min(lo[k],world[k]); hi[k]=std::max(hi[k],world[k]); }
    }
    finite(lo); finite(hi); grid.mWorldBBox=nv::Vec3dBBox(lo,hi);
    if(grid.hasMinMax()) {
      finite(root.minimum()); finite(root.maximum());
      summary.minimum=statistic(root.minimum()); summary.maximum=statistic(root.maximum());
    }
    if(grid.hasAverage()) { finite(root.average()); summary.average=double(root.average()); }
    if(grid.hasStdDeviation()) { finite(root.stdDeviation()); summary.standardDeviation=double(root.stdDeviation()); }
    const auto cached=[&](const auto& node) {
      if(node.bbox().empty()) return;
      if(grid.hasMinMax()) { finite(node.minimum()); finite(node.maximum()); }
      if(grid.hasAverage()) finite(node.average());
      if(grid.hasStdDeviation()) finite(node.stdDeviation());
    };
    for(auto it2=root.cbeginChild();it2;++it2) {
      cached(*it2);
      for(auto it1=it2->cbeginChild();it1;++it1) {
        cached(*it1);
        for(auto it0=it1->cbeginChild();it0;++it0) cached(*it0);
      }
    }
  }
  charge(work,plus(buffer.size()/2,1),l);
  grid.data()->mChecksum=detail::nano_integrity::evaluate(*grid.data(),nv::CheckMode::Full).checksum;
}
void update(nv::HostBuffer& buffer,NanoVdbStatisticsMode mode,
              NanoVdbStatisticsSummary& summary,uint64_t& work,const NanoVdbLimits& l) {
  const auto type=static_cast<const nv::GridData*>(buffer.data())->mGridType;
  switch(type) {
  case nv::GridType::Float: return updateTyped<float>(buffer,mode,summary,work,l);
  case nv::GridType::Double: return updateTyped<double>(buffer,mode,summary,work,l);
  case nv::GridType::Int32: return updateTyped<int32_t>(buffer,mode,summary,work,l);
  case nv::GridType::Int64: return updateTyped<int64_t>(buffer,mode,summary,work,l);
  case nv::GridType::Vec3f: return updateTyped<nv::Vec3f>(buffer,mode,summary,work,l);
  case nv::GridType::Vec3d: return updateTyped<nv::Vec3d>(buffer,mode,summary,work,l);
  case nv::GridType::Boolean: return updateTyped<bool>(buffer,mode,summary,work,l);
  case nv::GridType::Mask: return updateTyped<nv::ValueMask>(buffer,mode,summary,work,l);
  default: throw std::logic_error("unchecked type reached native statistics");
  }
}
#endif
} // namespace

NanoVdbStatisticsResult WriteNanoVdbStatistics(const std::filesystem::path& input,
    const std::filesystem::path& output,NanoVdbStatisticsMode mode,
    const NanoVdbReadOptions& read,const NanoVdbWriteOptions& write,const NanoVdbLimits& l) {
#if defined(CONTAINER_HAS_NANOVDB_IO) && CONTAINER_HAS_NANOVDB_IO
  check(mode>=NanoVdbStatisticsMode::Bounds && mode<=NanoVdbStatisticsMode::All,
        "invalid native statistics mode");
  check(!write.allowFloatMapRounding,"map rounding applies only to VDB conversion");
  check(write.codec>=NanoVdbCodec::None && write.codec<=NanoVdbCodec::Blosc &&
        write.checksum>=NanoVdbChecksum::Disabled && write.checksum<=NanoVdbChecksum::Full,
        "invalid native statistics codec/checksum");
  const auto capabilities=NanoVdbInterchangeCapabilities();
  check(write.codec!=NanoVdbCodec::Zip || capabilities.zip,"NanoVDB ZIP codec is unavailable");
  check(write.codec!=NanoVdbCodec::Blosc || capabilities.blosc,"NanoVDB BLOSC codec is unavailable");
  check(!write.raw || write.codec==NanoVdbCodec::None,"raw native statistics output cannot use compression");
  check(!output.empty() && !std::filesystem::exists(output),"native statistics output must be new");
  const uint64_t retained=plus(Fixed,times(l.maxGrids,plus(times(l.maxNameBytes,16),8192)));
  check(retained<l.maxWorkspaceBytes,"native statistics report/catalog reserve exceeds workspace budget");
  auto passLimits=l; passLimits.maxWorkspaceBytes-=retained;
  auto source=ReadNanoVdbFile(input,read,passLimits);
  NanoVdbStatisticsResult result;
  result.diagnostics=source.info().diagnostics;
  charge(result.work,source.info().work,l);
  result.summaries.reserve(source.info().grids.size());
  for(const auto& info:source.info().grids) {
    NanoVdbStatisticsSummary summary;
    summary.sourceIndex=info.index; summary.name=info.name; summary.valueType=info.valueType;
    summary.activeVoxels=info.activeVoxels; result.summaries.push_back(std::move(summary));
  }
  PrivateFiles files;
  auto intermediate=passLimits; intermediate.maxInputBytes=l.maxNativeBytes;
  intermediate.maxWork=l.maxWork-result.work;
  NanoVdbWriteOptions raw; raw.raw=true; raw.checksum=NanoVdbChecksum::Disabled;
  const auto staged=WriteNanoVdbFile(source,files.raw,raw,intermediate);
  charge(result.work,staged.work,l);
  source={};
  {
    std::ifstream in(files.raw,std::ios::binary);
    std::ofstream out(files.updated,std::ios::binary|std::ios::out|std::ios::noreplace);
    in.exceptions(std::ios::badbit|std::ios::failbit);
    out.exceptions(std::ios::badbit|std::ios::failbit);
#if defined(NANOVDB_USE_TBB)
    tbb::task_arena serial(1);
#endif
    for(size_t i=0;i<staged.grids.size();++i) {
      const auto bytes=staged.grids[i].nativeBytes;
      check(static_cast<long double>(bytes)+times(l.maxNodes,80)+times(l.maxMetadataBytes,16)+Fixed<=passLimits.maxWorkspaceBytes,
            "native statistics mutable buffer/workspace budget exceeded");
      auto buffer=nv::HostBuffer::create(bytes);
      in.read(static_cast<char*>(buffer.data()),std::streamsize(bytes));
#if defined(NANOVDB_USE_TBB)
      serial.execute([&] { update(buffer,mode,result.summaries[i],result.work,l); });
#else
      update(buffer,mode,result.summaries[i],result.work,l);
#endif
      out.write(static_cast<const char*>(buffer.data()),std::streamsize(bytes));
    }
    out.flush(); out.close();
  }
  intermediate.maxWork=l.maxWork-result.work;
  auto updated=ReadNanoVdbFile(files.updated,{},intermediate);
  charge(result.work,updated.info().work,l);
  result.diagnostics.emplace_back("Native active-value caches recomputed; original values, masks, Double maps and blind payloads retained.");
  result.diagnostics.emplace_back("Moments and vector squared-magnitude comparisons use upstream accumulation and native cache precision; they are approximate. Vector moments describe squared magnitudes.");
  if(std::any_of(result.summaries.begin(),result.summaries.end(),[](const auto& s) {
      return s.valueType=="bool" || s.valueType=="mask";
  })) result.diagnostics.emplace_back("Boolean/Mask grids expose occupancy bounds without numeric caches.");
  auto finalLimits=passLimits; finalLimits.maxWork=l.maxWork-result.work;
  static_assert(std::is_nothrow_move_assignable_v<NanoVdbFileInfo>);
  static_assert(std::is_nothrow_move_constructible_v<NanoVdbStatisticsResult>);
  result.file=WriteNanoVdbFile(updated,output,write,finalLimits);
  result.work+=result.file.work;
  return result;
#else
  (void)input; (void)output; (void)mode; (void)read; (void)write; (void)l;
  unavailableStats();
#endif
}
} // namespace container::geometry::vdb
