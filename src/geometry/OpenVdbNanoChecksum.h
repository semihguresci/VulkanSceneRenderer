#pragma once
// Private checksum compatibility for buffers whose complete pointer graph has
// already been bounded. No function in this header accepts unchecked input.
#include <nanovdb/tools/GridChecksum.h>
#include <array>
#include <stdexcept>

namespace container::geometry::vdb::detail::nano_integrity {
namespace nv=nanovdb;
struct Evaluation {
  nv::Checksum checksum;
  bool coversAllData{};
};
struct Verification {
  bool matches{}, coversAllData{}, legacyNodeTail{};
};
inline const nv::TreeData& tree(const nv::GridData& h) {
  return *reinterpret_cast<const nv::TreeData*>(reinterpret_cast<const uint8_t*>(&h)+sizeof(h));
}
inline uint64_t legacyHeadEnd(const nv::GridData& h) {
  const auto& t=tree(h);
  if(t.mNodeCount[2]) return sizeof(h)+uint64_t(t.mNodeOffset[2]);
  if(h.mBlindMetadataCount) return uint64_t(h.mBlindMetadataOffset);
  return h.mGridSize;
}
inline uint32_t blockedTail(const uint8_t* data,uint64_t size,const uint32_t* lut) {
  // Match the official 4 KiB block-table convention exactly, including its
  // zero-block case. Report that coverage gap; silently changing this digest
  // would make standard readers reject otherwise valid files.
  const uint64_t blocks=size>>12;
  uint32_t result=0;
  for(uint64_t i=0;i<blocks;++i) {
    const auto count=uint64_t(4096)+(i+1==blocks ? size-(blocks<<12) : 0);
    const uint32_t value=nv::util::crc32(data+(i<<12),size_t(count),lut);
    result=nv::util::crc32(&value,sizeof(value),lut,result);
  }
  return result;
}
template<class T>
inline uint32_t nodeTail(const nv::GridData& h,const uint32_t* lut) {
  const auto& t=tree(h);
  const auto* base=reinterpret_cast<const uint8_t*>(&h);
  const uint64_t widths[]{sizeof(nv::NanoLeaf<T>),sizeof(nv::NanoLower<T>),sizeof(nv::NanoUpper<T>)};
  uint32_t result=0;
  for(int level=2;level>=0;--level) {
    if(!t.mNodeCount[level]) continue;
    const auto* node=base+sizeof(h)+t.mNodeOffset[level];
    for(uint32_t i=0;i<t.mNodeCount[level];++i) {
      const uint32_t value=nv::util::crc32(node+i*widths[level],size_t(widths[level]),lut);
      result=nv::util::crc32(&value,sizeof(value),lut,result);
    }
  }
  return result;
}
inline uint32_t nodeTail(const nv::GridData& h,const uint32_t* lut) {
  switch(h.mGridType) {
  case nv::GridType::Float: return nodeTail<float>(h,lut);
  case nv::GridType::Double: return nodeTail<double>(h,lut);
  case nv::GridType::Int32: return nodeTail<int32_t>(h,lut);
  case nv::GridType::Int64: return nodeTail<int64_t>(h,lut);
  case nv::GridType::Boolean: return nodeTail<bool>(h,lut);
  case nv::GridType::Mask: return nodeTail<nv::ValueMask>(h,lut);
  case nv::GridType::Vec3f: return nodeTail<nv::Vec3f>(h,lut);
  case nv::GridType::Vec3d: return nodeTail<nv::Vec3d>(h,lut);
  default: throw std::logic_error("unchecked native type reached legacy CRC evaluation");
  }
}
inline Evaluation evaluate(const nv::GridData& h,nv::CheckMode mode,bool useNodeTail=false) {
  Evaluation result;
  if(mode==nv::CheckMode::Disable) return result;
  std::array<uint32_t,256> lut;
  nv::util::initCrc32Lut(lut.data());
  const bool legacy=h.mVersion<=nv::Version(32,6,0);
  const uint64_t headEnd=legacy ? legacyHeadEnd(h) : sizeof(h)+sizeof(nv::TreeData);
  const auto* base=reinterpret_cast<const uint8_t*>(&h);
  result.checksum.head()=nv::util::crc32(base+16,size_t(headEnd-16),lut.data());
  result.coversAllData=headEnd==h.mGridSize;
  if(mode==nv::CheckMode::Full) {
    if(legacy && useNodeTail) {
      result.checksum.tail()=nodeTail(h,lut.data());
      result.coversAllData=h.mBlindMetadataCount==0;
    } else {
      const auto bytes=h.mGridSize-headEnd;
      // Official ABI32.6 defaults downgrade an empty tail to header-only mode.
      if(!legacy || bytes) result.checksum.tail()=blockedTail(base+headEnd,bytes,lut.data());
      result.coversAllData=bytes==0 || bytes>=4096;
    }
  }
  return result;
}
inline Verification verify(const nv::GridData& h) {
  if(h.mChecksum.isEmpty()) return {};
  const auto mode=h.mChecksum.isHalf() ? nv::CheckMode::Half : nv::CheckMode::Full;
  const auto canonical=evaluate(h,mode);
  Verification result{canonical.checksum==h.mChecksum,canonical.coversAllData,false};
  if(!result.matches && h.mVersion<=nv::Version(32,6,0) && mode==nv::CheckMode::Full) {
    const auto old=evaluate(h,mode,true);
    if(old.checksum==h.mChecksum) result={true,old.coversAllData,true};
  }
  return result;
}
} // namespace container::geometry::vdb::detail::nano_integrity
