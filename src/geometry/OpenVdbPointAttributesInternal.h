#pragma once
#include "Container/geometry/OpenVdbPointAttributes.h"
#include "Container/geometry/OpenVdbGridSelection.h"
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
#include "OpenVdbFileUtils.h"
#include "OpenVdbMetadataCatalog.h"
#include <openvdb/openvdb.h>
#include <openvdb/points/PointDataGrid.h>
#include <openvdb/points/PointAttribute.h>
#include <openvdb/points/PointGroup.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>
#include <limits>
#include <memory>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <tuple>
#include <type_traits>
namespace container::geometry::vdb::point_attribute_detail {
namespace points=openvdb::points;
using Grid=points::PointDataGrid;
using Leaf=Grid::TreeType::LeafNodeType;
// File's writer makes a metadata/statistics view through virtual copyGrid().
// Preserve the same native tree and exact map instead of its simplifying copy.
class ExactWritePointGrid final: public Grid {
public:
  explicit ExactWritePointGrid(const Grid& source):Grid(openvdb::ConstPtrCast<Grid::TreeType>(source.constTreePtr())) {
    insertMeta(source); setTransform(detail::CopyTransformExact(source.transform()));
  }
  openvdb::GridBase::ConstPtr copyGrid() const override {
    return copyReplacingMetadataAndTransform(*this,detail::CopyTransformExact(transform()));
  }
  openvdb::GridBase::Ptr copyGrid() override {
    return openvdb::ConstPtrCast<openvdb::GridBase>(static_cast<const ExactWritePointGrid&>(*this).copyGrid());
  }
};
inline void writeExact(const std::filesystem::path& output,const Grid& grid,const ProcessingOptions& p) {
  detail::WriteFileExclusive(output,{std::make_shared<ExactWritePointGrid>(grid)},p.maxInputBytes);
}
constexpr long double Fixed=4.0L*1024*1024;
inline void budget(long double bytes,const ProcessingOptions& p,const char* why) {
  if (!std::isfinite(bytes) || bytes>p.maxInputBytes) throw std::invalid_argument(why);
}
inline void validateOptions(const ProcessingOptions& p,const PointAttributeOptions& o,bool writing=false) {
  if (!p.maxInputBytes || !p.maxVoxelCount || !o.maxPoints || uint64_t(o.maxPoints)>uint64_t(INT64_MAX) || !o.maxStringBytes ||
      !o.maxAttributes || o.maxAttributes>1024 || !o.maxGroups || o.maxGroups>4096 || !o.maxEdits || o.maxEdits>1024)
    throw std::invalid_argument("invalid point attribute count/string/memory limits (attributes<=1024, groups<=4096, edits<=1024)");
  if (writing && (p.gridName.empty() || p.gridName.size()>256 || p.gridName.find('\0')!=std::string::npos))
    throw std::invalid_argument("point attribute output name must contain 1..256 bytes without NUL");
}
inline void name(const std::string& n) {
  if (n.empty() || n.size()>256 || n.find('\0')!=std::string::npos)
    throw std::invalid_argument("point attribute/group names must contain 1..256 bytes without NUL");
}
inline long double metadataBytes(const openvdb::MetaMap& map) {
  long double bytes=sizeof(openvdb::MetaMap);
  for (auto it=map.beginMeta();it!=map.endMeta();++it)
    bytes+=2.0L*(it->first.size()+it->second->typeName().size()+it->second->size())+256;
  return bytes;
}
template<class T> struct Native {using Type=T; static T to(const T& x){return x;} static T from(const T& x){return x;}};
template<> struct Native<uint8_t> {using Type=bool; static bool to(uint8_t x){return x!=0;} static uint8_t from(bool x){return uint8_t(x);}};
template<> struct Native<std::array<float,3>> {using Type=openvdb::Vec3f; static Type to(const std::array<float,3>& x){return Type(x.data());} static std::array<float,3> from(const Type& x){return {x.x(),x.y(),x.z()};}};
template<> struct Native<std::array<double,3>> {using Type=openvdb::Vec3d; static Type to(const std::array<double,3>& x){return Type(x.data());} static std::array<double,3> from(const Type& x){return {x.x(),x.y(),x.z()};}};
template<class T> openvdb::NamePair nativeType() {
  if constexpr (std::is_same_v<T,std::string>) return points::StringAttributeArray::attributeType();
  else return points::TypedAttributeArray<typename Native<T>::Type,points::NullCodec>::attributeType();
}
template<class T> void registerType() {
  using Array=points::TypedAttributeArray<T,points::NullCodec>;
  if (!Array::isRegistered()) Array::registerType();
}
inline void initialize() {
  static std::once_flag once;
  std::call_once(once,[]{
    openvdb::initialize(); registerType<bool>(); registerType<int32_t>(); registerType<int64_t>();
    registerType<float>(); registerType<double>(); registerType<openvdb::Vec3f>(); registerType<openvdb::Vec3d>();
    if (!points::StringAttributeArray::isRegistered()) points::StringAttributeArray::registerType();
    if (!points::GroupAttributeArray::isRegistered()) points::GroupAttributeArray::registerType();
  });
}
inline int typeIndex(const openvdb::NamePair& type) {
  if (type==nativeType<uint8_t>()) return 0; if (type==nativeType<int32_t>()) return 1;
  if (type==nativeType<int64_t>()) return 2; if (type==nativeType<float>()) return 3;
  if (type==nativeType<double>()) return 4; if (type==nativeType<std::array<float,3>>()) return 5;
  if (type==nativeType<std::array<double,3>>()) return 6; if (type==nativeType<std::string>()) return 7; return -1;
}
struct Snapshot {
  std::filesystem::path directory,file;
  uintmax_t encoded=0;
  Snapshot(const std::filesystem::path& input,const ProcessingOptions& p) {
    if (!std::filesystem::is_regular_file(input)) throw std::invalid_argument("point attribute input is not a regular file");
    encoded=std::filesystem::file_size(input); budget(encoded+Fixed,p,"point attribute encoded snapshot exceeds memory budget");
    static std::atomic_uint64_t serial{0};
    for (unsigned attempt=0;attempt<64;++attempt) {
      directory=std::filesystem::temp_directory_path()/("container-point-attributes-"+
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+"-"+std::to_string(++serial));
      if (std::filesystem::create_directory(directory)) {file=directory/"source.vdb"; break;}
    }
    if (file.empty()) throw std::runtime_error("cannot create point attribute snapshot directory");
    try {
      std::ifstream source(input,std::ios::binary); std::ofstream target(file,std::ios::binary|std::ios::out|std::ios::noreplace);
      if (!source || !target) throw std::runtime_error("cannot open point attribute snapshot");
      std::array<char,64*1024> bytes{}; auto remaining=encoded;
      while (remaining) {
        const auto count=static_cast<std::streamsize>(std::min<uintmax_t>(remaining,bytes.size())); source.read(bytes.data(),count);
        if (source.gcount()!=count) throw std::runtime_error("point attribute input changed during snapshot");
        target.write(bytes.data(),count); remaining-=uintmax_t(count);
      }
      target.close(); if (!target || source.bad() || source.peek()!=std::char_traits<char>::eof())
        throw std::runtime_error("point attribute snapshot failed or input changed size");
    } catch (...) {cleanup(); throw;}
  }
  void cleanup() {std::error_code ignored; std::filesystem::remove(file,ignored); std::filesystem::remove(directory,ignored);}
  ~Snapshot(){cleanup();}
};
struct Loaded {
  std::unique_ptr<Snapshot> snapshot;
  Grid::Ptr grid;
  std::vector<const Leaf*> leaves;
  PointAttributeSchema schema;
  long double decoded=0, metadata=0;
  uintmax_t stringBytes=0,maxStringLength=0;
  size_t dictionaryEntries=0;
};
inline PointAttributeSchema describe(const Grid& grid,const std::vector<const Leaf*>& leaves,uint64_t count) {
  PointAttributeSchema result{grid.getName(),count,leaves.size()};
  if (leaves.empty()) return result;
  const auto& first=leaves.front()->attributeSet().descriptor();
  for (const auto& [n,index]:first.map()) {
    const auto& array=leaves.front()->constAttributeArray(index);
    PointAttributeDescription item{n,array.type().first,array.type().second,array.stride(),points::isGroup(array),false,false};
    for (const auto* leaf:leaves) {
      const auto& a=leaf->constAttributeArray(index); item.anyHidden|=a.isHidden();
      if (!a.hasConstantStride() || a.stride()!=item.stride) item.stride=0;
    }
    item.readable=!item.groupStorage && item.stride==1 && typeIndex(array.type())>=0;
    result.attributes.push_back(std::move(item));
  }
  for (const auto& [n,index]:first.groupMap()) result.groups.push_back(n);
  return result;
}
inline Loaded load(const std::filesystem::path& input,const std::string& selection,const ProcessingOptions& p,const PointAttributeOptions& o) {
  Loaded result; result.snapshot=std::make_unique<Snapshot>(input,p);
  const auto encoded=result.snapshot->encoded;
  detail::RequireSeekableFile(result.snapshot->file);
  if (!openvdb::io::Archive::isDelayedLoadingEnabled())
    throw std::invalid_argument("bounded point attribute editing requires delayed loading; unset OPENVDB_DISABLE_DELAYED_LOAD");
#ifndef OPENVDB_USE_DELAYED_LOADING
  throw std::invalid_argument("bounded point attribute editing requires OpenVDB delayed-loading support");
#else
  if (encoded>=std::numeric_limits<openvdb::Index64>::max()) throw std::invalid_argument("point snapshot exceeds delayed-copy size range");
  openvdb::io::File file(result.snapshot->file.string()); file.setCopyMaxBytes(openvdb::Index64(encoded)+1); file.open(true);
  std::map<std::string,size_t> descriptors; long double catalog=encoded+Fixed;
  for (auto it=file.beginName();it!=file.endName();++it) {
    const auto n=it.gridName(); catalog+=4.0L*n.size()+256;
    budget(catalog,p,"point attribute descriptor catalog exceeds memory budget");
    if (!descriptors.emplace(n,descriptors.size()).second)
      throw std::invalid_argument("point attribute input has colliding grid descriptors; rename grids first");
  }
  auto streamMetadata=std::make_shared<openvdb::io::StreamMetadata>(); streamMetadata->setSeekable(true);
  std::ifstream stream(result.snapshot->file,std::ios::binary); stream.exceptions(std::ios::badbit|std::ios::failbit);
  detail::ExactMetadataArchive archive; archive.begin(stream,streamMetadata);
  openvdb::MetaMap fileMetadata; fileMetadata.readMeta(stream); result.metadata=metadataBytes(fileMetadata);
  budget(catalog+8*result.metadata,p,"point attribute file/catalog metadata exceeds memory budget");
  const int32_t gridCount=archive.count(stream);
  if (gridCount<0 || size_t(gridCount)!=descriptors.size())
    throw std::invalid_argument("point attribute raw/native descriptor catalogs disagree");
  std::string selected; bool found=false; size_t selectedRank=SIZE_MAX;
  openvdb::math::Transform::Ptr exact;
  openvdb::GridBase::Ptr selectedHeader;
  for (int32_t index=0;index<gridCount;++index) {
    openvdb::io::GridDescriptor descriptor; const auto meta=archive.next(stream,descriptor,encoded);
    const auto n=openvdb::io::GridDescriptor::nameAsString(descriptor.uniqueName());
    const auto rank=descriptors.find(n);
    if(rank==descriptors.end()) throw std::invalid_argument("point attribute raw/native descriptor names disagree");
    result.metadata+=metadataBytes(*meta);
    budget(catalog+8*result.metadata,p,"point attribute aggregate scanned metadata exceeds memory budget");
    if (!meta->isType<Grid>() || !GridSelectionMatches(selection,n,meta->getName())) continue;
    if (selection.empty() && found && rank->second>=selectedRank) continue;
    if (found && !selection.empty()) throw std::invalid_argument("point attribute grid is ambiguous; use descriptor:<exact name>");
    found=true; selected=n; selectedRank=rank->second; selectedHeader=meta;
  }
  if (!found) throw std::invalid_argument("no matching PointDataGrid found for native attributes");
  {
    const auto memory=selectedHeader->getMetadata<openvdb::Int64Metadata>(openvdb::GridBase::META_FILE_MEM_BYTES);
    const auto voxels=selectedHeader->getMetadata<openvdb::Int64Metadata>(openvdb::GridBase::META_FILE_VOXEL_COUNT);
    if (!memory || !voxels || memory->value()<0 || voxels->value()<0 || uint64_t(voxels->value())>p.maxVoxelCount)
      throw std::invalid_argument("point attributes require valid recorded file memory/voxel statistics within budget");
    budget(catalog+memory->value()+8*result.metadata,p,"recorded point attribute topology exceeds memory budget");
    exact=detail::CopyTransformExact(selectedHeader->transform());
  }
  if (file.getUniqueTag()!=archive.getUniqueTag()) throw std::runtime_error("point attribute private snapshot changed");
  stream.close(); result.grid=openvdb::gridPtrCast<Grid>(file.readGrid(selected)); file.close();
  result.grid->setTransform(exact);
  const auto& grid=*result.grid;
  const auto maxLeaves=p.maxVoxelCount/512+(p.maxVoxelCount%512!=0);
  if (grid.tree().hasActiveTiles() || grid.activeVoxelCount()>p.maxVoxelCount || grid.tree().leafCount()>maxLeaves)
    throw std::invalid_argument("native point topology exceeds voxel/leaf limits or contains active tiles");
  result.metadata+=metadataBytes(grid); result.decoded=grid.memUsage();
  budget(catalog+result.decoded+4*result.metadata,p,"loaded point attribute tree exceeds memory budget");
  const points::AttributeSet::Descriptor* first=nullptr; uint64_t count=0;
  for (auto leaf=grid.tree().cbeginLeaf();leaf;++leaf) {
    const auto& descriptor=leaf->attributeSet().descriptor();
    if (!first) first=&descriptor;
    else if (descriptor!=*first) throw std::invalid_argument("native point editing requires identical descriptors/metadata across leaves; heterogeneous descriptors cannot be merged losslessly");
    if (descriptor.map().size()>o.maxAttributes || descriptor.groupMap().size()>o.maxGroups || !leaf->hasAttribute("P"))
      throw std::invalid_argument("native point schema exceeds attribute/group limits or lacks mandatory P");
    std::set<size_t> groupOffsets;
    for (const auto& [n,offset]:descriptor.groupMap()) {
      const auto index=leaf->attributeSet().groupIndex(n);
      if (index.first>=leaf->attributeSet().size() || !points::isGroup(leaf->constAttributeArray(index.first)) ||
          !groupOffsets.insert(offset).second)
        throw std::invalid_argument("point groups have invalid or aliased backing bits; independent edits cannot preserve aliases");
      const auto& backing=leaf->constAttributeArray(index.first);
      if (!backing.hasConstantStride() || backing.stride()!=1)
        throw std::invalid_argument("referenced point group backing arrays must have constant stride one; native group handles do not apply attribute strides");
    }
    leaf->validateOffsets(); const auto leafCount=leaf->pointCount();
    if (leafCount>o.maxPoints-std::min<uint64_t>(count,o.maxPoints)) throw std::invalid_argument("native point count exceeds maxPoints");
    count+=leafCount; result.metadata+=metadataBytes(descriptor.getMetadata());
    const auto& position=leaf->constAttributeArray("P");
    if (!position.hasValueType<openvdb::Vec3f>() || !position.hasConstantStride() || position.stride()!=1)
      throw std::invalid_argument("mandatory P must be a stride-one Vec3f attribute");
    for (size_t index=0;index<leaf->attributeSet().size();++index) {
      const auto& array=leaf->constAttributeArray(index);
      if (array.isTransient() || array.isStreaming()) throw std::invalid_argument("transient/streaming point payloads cannot be preserved by the file writer");
      if (array.size()!=leafCount) throw std::invalid_argument("point attribute length disagrees with native offsets");
      result.decoded+=array.memUsageIfLoaded();
    }
    result.leaves.push_back(&*leaf);
  }
  std::sort(result.leaves.begin(),result.leaves.end(),[](const Leaf* a,const Leaf* b){
    return std::tuple{a->origin().x(),a->origin().y(),a->origin().z()}<std::tuple{b->origin().x(),b->origin().y(),b->origin().z()};
  });
  if (first) for (auto it=first->getMetadata().beginMeta();it!=first->getMetadata().endMeta();++it) {
    if (!it->first.starts_with("string:")) continue;
    const auto value=std::dynamic_pointer_cast<openvdb::StringMetadata>(it->second);
    if (!value) throw std::invalid_argument("point string dictionary has incompatible metadata");
    if (value->value().size()>o.maxStringBytes-result.stringBytes) throw std::invalid_argument("source point string dictionary exceeds maxStringBytes");
    result.stringBytes+=value->value().size(); result.maxStringLength=std::max<uintmax_t>(result.maxStringLength,value->value().size()); ++result.dictionaryEntries;
  }
  budget(catalog+3*result.decoded+4*result.metadata+result.leaves.size()*64.0L,p,
         "point attribute decoded/dictionary workspace exceeds memory budget");
  result.schema=describe(grid,result.leaves,count); return result;
#endif
}
template<class T> void finite(const T& x) {
  if constexpr (std::is_same_v<T,uint8_t>) {if (x>1) throw std::invalid_argument("boolean point values must be 0/1");}
  else if constexpr (std::is_floating_point_v<T>) {if (!std::isfinite(x)) throw std::invalid_argument("point numeric attribute values must be finite");}
  else if constexpr (std::is_same_v<T,std::array<float,3>> || std::is_same_v<T,std::array<double,3>>) {
    for (auto c:x) if (!std::isfinite(c)) throw std::invalid_argument("point vector attribute values must be finite");
  }
}
} // namespace container::geometry::vdb::point_attribute_detail
#endif
