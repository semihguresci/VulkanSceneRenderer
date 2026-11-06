#include "Container/geometry/OpenVdbPointAttributes.h"
#include "OpenVdbPointAttributesInternal.h"
#include "Container/geometry/OpenVdbGridSelection.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <type_traits>

#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
#include "OpenVdbFileUtils.h"
#include <openvdb/openvdb.h>
#include <openvdb/points/PointDataGrid.h>
#include <openvdb/points/PointAttribute.h>
#include <openvdb/points/PointGroup.h>
#include <tbb/task_arena.h>
#include <array>
#include <atomic>
#include <chrono>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <string_view>
#include <tuple>
#endif

namespace container::geometry::vdb {
namespace {
[[noreturn]] void unavailable() {throw std::runtime_error("OpenVDB point attributes are unavailable; configure with -DENABLE_OPENVDB=ON.");}
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
using namespace point_attribute_detail;
template<class T> PointAttributeValues readColumn(const Loaded& source,const std::string& attribute,const ProcessingOptions& p,const PointAttributeOptions& o) {
  const auto count=source.schema.pointCount;
  const long double strings=std::is_same_v<T,std::string>?std::min<long double>(o.maxStringBytes,static_cast<long double>(count)*source.maxStringLength):0;
  budget(source.snapshot->encoded+3*source.decoded+4*source.metadata+count*(3.0L*sizeof(T))+3*strings+Fixed,p,
         "point attribute readback column exceeds memory budget");
  std::vector<T> result; result.reserve(size_t(count)); uintmax_t bytes=0;
  for (const auto* leaf:source.leaves) {
    const auto& array=leaf->constAttributeArray(attribute);
    if constexpr (std::is_same_v<T,std::string>) {
      points::StringAttributeHandle handle(array,leaf->attributeSet().descriptor().getMetadata());
      for (openvdb::Index n=0;n<leaf->pointCount();++n) {
        auto value=handle.get(n); if (value.size()>o.maxStringBytes-bytes) throw std::invalid_argument("decoded point string column exceeds maxStringBytes");
        bytes+=value.size(); result.push_back(std::move(value));
      }
    } else {
      points::AttributeHandle<typename Native<T>::Type,points::NullCodec> handle(array);
      for (openvdb::Index n=0;n<leaf->pointCount();++n) {auto value=Native<T>::from(handle.get(n)); finite(value); result.push_back(value);}
    }
  }
  return result;
}
struct SimAttribute {openvdb::NamePair type; uint32_t stride; bool group;};
struct EditReserve {long double bytes=0; uintmax_t strings=0;};
EditReserve validateEdits(const Loaded& source,std::span<const PointAttributeEdit> edits,std::span<const PointGroupEdit> groups,
                          const ProcessingOptions& p,const PointAttributeOptions& o) {
  if (edits.size()>o.maxEdits || groups.size()>o.maxEdits-edits.size()) throw std::invalid_argument("native point edit batch exceeds maxEdits");
  if (source.leaves.empty() && (!edits.empty() || !groups.empty())) throw std::invalid_argument("empty PointDataGrid has no persisted descriptor; add points before attribute/group edits");
  std::map<std::string,SimAttribute> schema; std::set<std::string> groupNames;
  for (const auto& a:source.schema.attributes) schema.emplace(a.name,SimAttribute{{a.valueType,a.codec},a.stride,a.groupStorage});
  for (const auto& n:source.schema.groups) groupNames.insert(n);
  // appendGroup may reuse an orphaned backing array even when no existing
  // group refers to it. Check these arrays before any native group mutation.
  if (!groups.empty()) for (const auto* leaf:source.leaves) for (size_t n=0;n<leaf->attributeSet().size();++n) {
    const auto& a=leaf->constAttributeArray(n);
    if (points::isGroup(a) && (!a.hasConstantStride() || a.stride()!=1))
      throw std::invalid_argument("native group edits require constant stride-one backing arrays, including unused group storage");
  }
  EditReserve reserve; reserve.strings=source.stringBytes;
  size_t groupCapacity=8*std::count_if(source.schema.attributes.begin(),source.schema.attributes.end(),[](const auto& a){return a.groupStorage;});
  size_t extraGroupArrays=0;
  for (const auto& edit:edits) {
    name(edit.name);
    if (edit.name=="P" || edit.newName=="P") throw std::invalid_argument("P is protected from every mutation; coordinate edits require point repartitioning");
    const auto found=schema.find(edit.name);
    if (edit.operation==PointEditOperation::Add || edit.operation==PointEditOperation::Set) {
      if (!edit.newName.empty()) throw std::invalid_argument("Add/Set must not specify a rename destination");
      if (edit.name.starts_with("__group")) throw std::invalid_argument("__group names are reserved for native group storage");
      if ((edit.operation==PointEditOperation::Add)==(found!=schema.end())) throw std::invalid_argument("attribute Add requires a missing name; Set requires an existing name");
      std::visit([&](const auto& values) {
        using T=typename std::decay_t<decltype(values)>::value_type;
        if (values.size()!=1 && values.size()!=source.schema.pointCount) throw std::invalid_argument("point attribute payload must contain one broadcast value or exactly pointCount values");
        for (const auto& value:values) finite(value);
        const auto type=nativeType<T>();
        if (found!=schema.end() && (found->second.group || found->second.stride!=1 || found->second.type!=type))
          throw std::invalid_argument("Set requires matching supported type/codec and constant stride one; no implicit codec conversion");
        reserve.bytes+=values.size()*static_cast<long double>(sizeof(T));
        const size_t width=std::is_same_v<T,std::string>?sizeof(openvdb::Index):sizeof(typename Native<T>::Type);
        reserve.bytes+=source.schema.pointCount*(4.0L*width)+source.leaves.size()*512.0L;
        if constexpr (std::is_same_v<T,std::string>) for (const auto& value:values) {
          if (value.size()>o.maxStringBytes-reserve.strings) throw std::invalid_argument("source dictionary plus edit strings exceed maxStringBytes");
          reserve.strings+=value.size(); reserve.bytes+=4.0L*value.size()+512;
        }
        if (edit.operation==PointEditOperation::Add) schema.emplace(edit.name,SimAttribute{type,1,false});
      },edit.values);
    } else if (edit.operation==PointEditOperation::Rename || edit.operation==PointEditOperation::Drop) {
      if (std::visit([](const auto& v){return !v.empty();},edit.values)) throw std::invalid_argument("Rename/Drop must not contain attribute values");
      if (found==schema.end() || found->second.group) throw std::invalid_argument("attribute Rename/Drop needs an existing non-group attribute");
      if (edit.operation==PointEditOperation::Rename) {
        name(edit.newName);
        if (edit.newName.starts_with("__group") || schema.contains(edit.newName)) throw std::invalid_argument("attribute rename destination exists or is reserved");
        schema.emplace(edit.newName,found->second);
      } else if (!edit.newName.empty()) throw std::invalid_argument("Drop must not specify a destination");
      schema.erase(found);
    } else throw std::invalid_argument("unknown native attribute edit operation");
    reserve.bytes+=2.0L*(edit.name.size()+edit.newName.size())+512;
    if (schema.size()>o.maxAttributes) throw std::invalid_argument("point edits exceed maxAttributes");
  }
  for (const auto& edit:groups) {
    name(edit.name);
    if (edit.operation==PointEditOperation::Add || edit.operation==PointEditOperation::Set) {
      if (!edit.newName.empty()) throw std::invalid_argument("group Add/Set must not specify a destination");
      if ((edit.operation==PointEditOperation::Add)==groupNames.contains(edit.name)) throw std::invalid_argument("group Add needs missing name; Set needs existing name");
      if (edit.membership.size()!=1 && edit.membership.size()!=source.schema.pointCount) throw std::invalid_argument("group payload must contain one broadcast value or exactly pointCount values");
      for (auto value:edit.membership) finite(value);
      if (edit.operation==PointEditOperation::Add) {
        groupNames.insert(edit.name);
        if (groupNames.size()>groupCapacity) {groupCapacity+=8; ++extraGroupArrays;}
      }
      reserve.bytes+=edit.membership.size()+source.schema.pointCount*4.0L+source.leaves.size()*512.0L;
    } else if (edit.operation==PointEditOperation::Rename || edit.operation==PointEditOperation::Drop) {
      if (!edit.membership.empty() || !groupNames.contains(edit.name)) throw std::invalid_argument("group Rename/Drop needs an existing name and no payload");
      if (edit.operation==PointEditOperation::Rename) {name(edit.newName); if (groupNames.contains(edit.newName)) throw std::invalid_argument("group rename destination exists"); groupNames.insert(edit.newName);}
      else if (!edit.newName.empty()) throw std::invalid_argument("group Drop must not specify a destination");
      groupNames.erase(edit.name);
    } else throw std::invalid_argument("unknown native group edit operation");
    reserve.bytes+=2.0L*(edit.name.size()+edit.newName.size())+512;
    if (groupNames.size()>o.maxGroups) throw std::invalid_argument("point edits exceed maxGroups");
    if (schema.size()+extraGroupArrays>o.maxAttributes) throw std::invalid_argument("group backing storage exceeds maxAttributes");
  }
  budget(source.snapshot->encoded+4*source.decoded+8*source.metadata+4*reserve.bytes+
         source.leaves.size()*128.0L+Fixed,p,"native point clone/edit/string workspace exceeds memory budget");
  return reserve;
}
std::vector<Leaf*> sortedLeaves(Grid& grid) {
  std::vector<Leaf*> leaves; leaves.reserve(grid.tree().leafCount());
  for (auto leaf=grid.tree().beginLeaf();leaf;++leaf) leaves.push_back(&*leaf);
  std::sort(leaves.begin(),leaves.end(),[](const Leaf* a,const Leaf* b){return std::tuple{a->origin().x(),a->origin().y(),a->origin().z()}<std::tuple{b->origin().x(),b->origin().y(),b->origin().z()};});
  return leaves;
}
template<class T> bool sameBits(const T& a,const T& b) {
  if constexpr (std::is_same_v<T,float>) return std::bit_cast<uint32_t>(a)==std::bit_cast<uint32_t>(b);
  else if constexpr (std::is_same_v<T,double>) return std::bit_cast<uint64_t>(a)==std::bit_cast<uint64_t>(b);
  else if constexpr (std::is_same_v<T,std::array<float,3>> || std::is_same_v<T,std::array<double,3>>) {
    return sameBits(a[0],b[0]) && sameBits(a[1],b[1]) && sameBits(a[2],b[2]);
  } else return a==b;
}
template<class T> void assign(Grid& grid,const std::vector<Leaf*>& leaves,const PointAttributeEdit& edit,const std::vector<T>& values) {
  if (edit.operation==PointEditOperation::Add) points::appendAttribute<typename Native<T>::Type>(grid.tree(),edit.name);
  if constexpr (std::is_same_v<T,std::string>) {
    const auto descriptor=points::makeDescriptorUnique(grid.tree());
    {points::StringMetaInserter inserter(descriptor->getMetadata()); for (const auto& value:values) inserter.insert(value);}
  }
  size_t offset=0;
  for (auto* leaf:leaves) {
    if constexpr (std::is_same_v<T,std::string>) {
      points::StringAttributeWriteHandle handle(leaf->attributeArray(edit.name),leaf->attributeSet().descriptor().getMetadata());
      for (openvdb::Index n=0;n<leaf->pointCount();++n) handle.set(n,values[values.size()==1?0:offset+n]);
      handle.compact();
    } else {
      points::AttributeWriteHandle<typename Native<T>::Type,points::NullCodec> handle(leaf->attributeArray(edit.name));
      for (openvdb::Index n=0;n<leaf->pointCount();++n) handle.set(n,Native<T>::to(values[values.size()==1?0:offset+n]));
      // Native numeric compaction compares decoded equality, which conflates
      // signed zeros. Only compact an explicitly bit-identical leaf payload.
      bool uniform=true;
      if (values.size()!=1) for (openvdb::Index n=1;n<leaf->pointCount();++n)
        uniform&=sameBits(values[offset],values[offset+n]);
      if (uniform) handle.compact();
    }
    offset+=leaf->pointCount();
  }
}
void structural(const Loaded& source,const Grid& target,const PointAttributeOptions& o,const ProcessingOptions& p,long double reserve) {
  if (source.grid->transform()!=target.transform() || source.grid->getGridClass()!=target.getGridClass() || !source.grid->tree().hasSameTopology(target.tree()))
    throw std::runtime_error("native point attribute editing unexpectedly changed transform/class/topology");
  long double bytes=target.memUsage()+metadataBytes(target);
  for (const auto* original:source.leaves) {
    const auto* leaf=target.tree().probeConstLeaf(original->origin());
    if (!leaf || leaf->pointCount()!=original->pointCount() || leaf->attributeSet().size()>o.maxAttributes || leaf->attributeSet().descriptor().groupMap().size()>o.maxGroups)
      throw std::runtime_error("native point editing changed offsets or exceeded schema limits");
    leaf->validateOffsets();
    for (openvdb::Index n=0;n<Leaf::NUM_VALUES;++n) if (leaf->getValue(n)!=original->getValue(n)) throw std::runtime_error("native point editing changed point offset buffers");
    bytes+=metadataBytes(leaf->attributeSet().descriptor().getMetadata());
    for (size_t index=0;index<leaf->attributeSet().size();++index) {
      const auto& a=leaf->constAttributeArray(index);
      if (a.isTransient() || a.isStreaming() || a.size()!=leaf->pointCount()) throw std::invalid_argument("edited attributes contain unpreservable flags or invalid lengths");
      bytes+=a.memUsageIfLoaded();
    }
  }
  budget(source.snapshot->encoded+source.decoded+4*source.metadata+3*bytes+reserve+Fixed,p,
         "edited native point output/writer workspace exceeds memory budget");
}
#endif
} // namespace

PointAttributeSchema InspectPointAttributes(const std::filesystem::path& input,const std::string& selector,const ProcessingOptions& p,const PointAttributeOptions& o) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  validateOptions(p,o); initialize(); tbb::task_arena arena(1);
  return arena.execute([&]{return load(input,selector,p,o).schema;});
#else
  (void)input; (void)selector; (void)p; (void)o; unavailable();
#endif
}
PointAttributeColumn ReadPointAttribute(const std::filesystem::path& input,const std::string& attribute,const std::string& selector,const ProcessingOptions& p,const PointAttributeOptions& o) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  validateOptions(p,o); name(attribute); initialize(); tbb::task_arena arena(1);
  return arena.execute([&] {
    const auto source=load(input,selector,p,o);
    const auto found=std::find_if(source.schema.attributes.begin(),source.schema.attributes.end(),[&](const auto& a){return a.name==attribute;});
    if (found==source.schema.attributes.end() || !found->readable) throw std::invalid_argument("selected point attribute is missing, a group array, or not a supported stride-one type/codec");
    PointAttributeValues values;
    switch (typeIndex({found->valueType,found->codec})) {
    case 0: values=readColumn<uint8_t>(source,attribute,p,o); break;
    case 1: values=readColumn<int32_t>(source,attribute,p,o); break;
    case 2: values=readColumn<int64_t>(source,attribute,p,o); break;
    case 3: values=readColumn<float>(source,attribute,p,o); break;
    case 4: values=readColumn<double>(source,attribute,p,o); break;
    case 5: values=readColumn<std::array<float,3>>(source,attribute,p,o); break;
    case 6: values=readColumn<std::array<double,3>>(source,attribute,p,o); break;
    case 7: values=readColumn<std::string>(source,attribute,p,o); break;
    default: throw std::invalid_argument("unsupported native point attribute");
    }
    return PointAttributeColumn{*found,std::move(values)};
  });
#else
  (void)input; (void)attribute; (void)selector; (void)p; (void)o; unavailable();
#endif
}
std::vector<uint8_t> ReadPointGroup(const std::filesystem::path& input,const std::string& group,const std::string& selector,const ProcessingOptions& p,const PointAttributeOptions& o) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  validateOptions(p,o); name(group); initialize(); tbb::task_arena arena(1);
  return arena.execute([&] {
    const auto source=load(input,selector,p,o);
    if (std::find(source.schema.groups.begin(),source.schema.groups.end(),group)==source.schema.groups.end()) throw std::invalid_argument("point group was not found");
    budget(source.snapshot->encoded+3*source.decoded+4*source.metadata+source.schema.pointCount*3.0L+Fixed,p,"point group readback exceeds memory budget");
    std::vector<uint8_t> result; result.reserve(size_t(source.schema.pointCount));
    for (const auto* leaf:source.leaves) {const auto handle=leaf->groupHandle(group); for (openvdb::Index n=0;n<leaf->pointCount();++n) result.push_back(uint8_t(handle.get(n)));}
    return result;
  });
#else
  (void)input; (void)group; (void)selector; (void)p; (void)o; unavailable();
#endif
}
PointAttributeSchema WritePointAttributeEdits(const std::filesystem::path& input,const std::filesystem::path& output,
    std::span<const PointAttributeEdit> edits,std::span<const PointGroupEdit> groups,const std::string& selector,
    const ProcessingOptions& p,const PointAttributeOptions& o) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  validateOptions(p,o,true);
  if (output.empty() || std::filesystem::exists(output) || (output.has_parent_path() && !std::filesystem::is_directory(output.parent_path())))
    throw std::invalid_argument("point attribute output must be a new path in an existing directory");
  initialize(); tbb::task_arena arena(1);
  return arena.execute([&] {
    const auto source=load(input,selector,p,o); const auto reserve=validateEdits(source,edits,groups,p,o);
    auto target=detail::DeepCopyGridExact(*source.grid); auto leaves=sortedLeaves(*target);
    for (const auto& edit:edits) {
      if (edit.operation==PointEditOperation::Add || edit.operation==PointEditOperation::Set)
        std::visit([&](const auto& values){assign(*target,leaves,edit,values);},edit.values);
      else if (edit.operation==PointEditOperation::Rename) points::renameAttribute(target->tree(),edit.name,edit.newName);
      else points::dropAttribute(target->tree(),edit.name);
    }
    for (const auto& edit:groups) {
      if (edit.operation==PointEditOperation::Add || edit.operation==PointEditOperation::Set) {
        if (edit.operation==PointEditOperation::Add) points::appendGroup(target->tree(),edit.name);
        size_t offset=0;
        for (auto* leaf:leaves) {
          auto handle=leaf->groupWriteHandle(edit.name);
          for (openvdb::Index n=0;n<leaf->pointCount();++n) handle.set(n,edit.membership[edit.membership.size()==1?0:offset+n]!=0);
          handle.compact(); offset+=leaf->pointCount();
        }
      } else if (edit.operation==PointEditOperation::Rename) points::makeDescriptorUnique(target->tree())->renameGroup(edit.name,edit.newName);
      else points::dropGroup(target->tree(),edit.name,false);
    }
    target->setName(p.gridName);
    // This producer cache is regenerated from validated native offsets. Foreign
    // writers can reuse its key with another type; replace it explicitly.
    target->removeMeta("container:point_count");
    target->insertMeta("container:point_count",openvdb::Int64Metadata(int64_t(source.schema.pointCount)));
    structural(source,*target,o,p,reserve.bytes);
    std::vector<const Leaf*> resultLeaves(leaves.begin(),leaves.end());
    auto result=describe(*target,resultLeaves,source.schema.pointCount);
    writeExact(output,*target,p);
    return result;
  });
#else
  (void)input; (void)output; (void)edits; (void)groups; (void)selector; (void)p; (void)o; unavailable();
#endif
}
} // namespace container::geometry::vdb
