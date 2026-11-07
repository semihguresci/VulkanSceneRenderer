#include "Container/geometry/OpenVdbPointSelection.h"
#include <stdexcept>

#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
#include "OpenVdbPointAttributesInternal.h"
#include "OpenVdbExactStatistics.h"
#include <tbb/task_arena.h>
#include <bit>
#include <optional>
#include <sstream>
#include <string_view>
#endif

namespace container::geometry::vdb {
namespace {
[[noreturn]] void unavailable(){throw std::runtime_error("OpenVDB point selection/statistics are unavailable; configure with -DENABLE_OPENVDB=ON.");}
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
using namespace point_attribute_detail;
constexpr std::array<const char*,3> ArchiveKeys{
  "container:point_empty_manifest_v1","container:point_empty_descriptor_v1","container:point_empty_leaf_flags_v1"};
constexpr std::string_view ManifestMagic="Container.PointEmpty.Manifest.1";
constexpr std::string_view FlagsMagic="Container.PointEmpty.LeafFlags.1";
uint64_t checksum(std::string_view data) {
  uint64_t value=14695981039346656037ull;for(unsigned char byte:data){value^=byte;value*=1099511628211ull;}return value;
}
struct Bytes {
  std::string bytes;size_t maximum;
  explicit Bytes(size_t cap):maximum(cap){}
  void put(std::string_view value){if(value.size()>maximum-bytes.size())throw std::invalid_argument("point empty-schema archive exceeds its preflight byte limit");bytes.append(value);}
  void u64(uint64_t value){char data[8];for(size_t i=0;i<8;++i)data[i]=char(value>>(8*i));put({data,8});}
  void u32(uint32_t value){char data[4];for(size_t i=0;i<4;++i)data[i]=char(value>>(8*i));put({data,4});}
  void byte(uint8_t value){const char b=char(value);put({&b,1});}
  void string(std::string_view value){if(value.size()>UINT32_MAX)throw std::invalid_argument("point archive field exceeds native string range");u32(uint32_t(value.size()));put(value);}
};
struct Reader {
  std::string_view bytes;
  std::string_view take(size_t n){if(n>bytes.size())throw std::invalid_argument("truncated point empty-schema archive");auto result=bytes.substr(0,n);bytes.remove_prefix(n);return result;}
  uint64_t u64(){uint64_t value=0;const auto b=take(8);for(size_t i=0;i<8;++i)value|=uint64_t(uint8_t(b[i]))<<(8*i);return value;}
  uint32_t u32(){const auto value=take(4);uint32_t result=0;for(size_t i=0;i<4;++i)result|=uint32_t(uint8_t(value[i]))<<(8*i);return result;}
  uint8_t byte(){return uint8_t(take(1)[0]);}
  std::string string(){const auto n=u32();return std::string(take(n));}
  void end(){if(!bytes.empty())throw std::invalid_argument("point empty-schema archive has trailing bytes");}
};
class DescriptorBuffer final:public std::streambuf {
 public:
  explicit DescriptorBuffer(size_t cap):bytes(cap){}Bytes bytes;
 protected:
  std::streamsize xsputn(const char* data,std::streamsize size)override{if(size<0)return 0;bytes.put({data,size_t(size)});return size;}
  int_type overflow(int_type value)override{if(traits_type::eq_int_type(value,traits_type::eof()))return traits_type::not_eof(value);const char c=traits_type::to_char_type(value);bytes.put({&c,1});return value;}
};
struct Archive {bool present=false;PointAttributeSchema schema;};
Archive inspectArchive(const Loaded& source,const PointAttributeOptions& o,const ProcessingOptions& p) {
  Archive result;size_t present=0;for(const auto* key:ArchiveKeys)if((*source.grid)[key])++present;
  if(!present)return result;
  if(!source.leaves.empty() || source.schema.pointCount || present!=ArchiveKeys.size())
    throw std::invalid_argument("reserved point empty-schema archive keys conflict with native points or are incomplete");
  std::array<openvdb::StringMetadata::ConstPtr,3> values;
  long double size=0;
  for(size_t i=0;i<3;++i){values[i]=source.grid->getMetadata<openvdb::StringMetadata>(ArchiveKeys[i]);if(!values[i])throw std::invalid_argument("point empty-schema archive metadata has the wrong type");size+=values[i]->value().size();}
  budget(source.snapshot->encoded+3*source.decoded+8*source.metadata+4*size+Fixed,p,"point empty-schema inspection exceeds shared memory budget");
  Reader input{values[0]->value()};if(input.take(ManifestMagic.size())!=ManifestMagic || input.u32()!=12)
    throw std::invalid_argument("unsupported point empty-schema archive version (requires version1/OpenVDB12)");
  const auto attributeCount=input.u64(),groupCount=input.u64(),leafCount=input.u64(),originalCount=input.u64(),dictionaryBytes=input.u64();
  if(!attributeCount || !leafCount || !originalCount || attributeCount>o.maxAttributes || groupCount>o.maxGroups || originalCount>o.maxPoints ||
     dictionaryBytes>o.maxStringBytes || leafCount>p.maxVoxelCount/512+(p.maxVoxelCount%512!=0))
    throw std::invalid_argument("point empty-schema archive exceeds attribute/group/point/topology limits");
  const auto descriptorSize=input.u64(),descriptorHash=input.u64(),flagSize=input.u64(),flagHash=input.u64();
  if(!descriptorSize || descriptorSize!=values[1]->value().size() || flagSize!=values[2]->value().size() ||
     descriptorHash!=checksum(values[1]->value()) || flagHash!=checksum(values[2]->value()))
    throw std::invalid_argument("point empty-schema archive byte length/checksum mismatch");
  result.schema.gridName=source.grid->getName();std::set<std::string> names,groups;
  for(uint64_t n=0;n<attributeCount;++n) {
    PointAttributeDescription a;a.name=input.string();a.valueType=input.string();a.codec=input.string();a.stride=input.u32();const auto flags=input.byte();
    if(flags&~uint8_t(7) || !names.insert(a.name).second)throw std::invalid_argument("invalid point empty-schema attribute manifest");
    a.groupStorage=flags&1;a.anyHidden=flags&2;a.readable=flags&4;
    const bool groupStorage=openvdb::NamePair{a.valueType,a.codec}==points::GroupAttributeArray::attributeType();
    if(groupStorage!=a.groupStorage)throw std::invalid_argument("point empty-schema group storage/type mismatch");
    const bool readable=!a.groupStorage && a.stride==1 && typeIndex({a.valueType,a.codec})>=0;
    if(a.readable!=readable)throw std::invalid_argument("point empty-schema readable/type manifest mismatch");
    result.schema.attributes.push_back(std::move(a));
  }
  const auto position=std::find_if(result.schema.attributes.begin(),result.schema.attributes.end(),[](const auto& a){return a.name=="P";});
  if(position==result.schema.attributes.end() || position->valueType!=nativeType<std::array<float,3>>().first || position->stride!=1 ||
     groupCount>8*std::count_if(result.schema.attributes.begin(),result.schema.attributes.end(),[](const auto& a){return a.groupStorage;}))
    throw std::invalid_argument("point empty-schema lacks canonical P or enough group storage");
  for(uint64_t n=0;n<groupCount;++n){auto group=input.string();if(!groups.insert(group).second)throw std::invalid_argument("duplicate point empty-schema group");result.schema.groups.push_back(std::move(group));}
  input.end();Reader flags{values[2]->value()};if(flags.take(FlagsMagic.size())!=FlagsMagic || flags.u64()!=leafCount || flags.u64()!=attributeCount)
    throw std::invalid_argument("invalid point empty-schema leaf flag header");
  std::vector<uint8_t> hidden(size_t(attributeCount),0);std::vector<uint32_t> strides(size_t(attributeCount),0);std::optional<std::array<int32_t,3>> previousOrigin;
  for(uint64_t leaf=0;leaf<leafCount;++leaf) {
    std::array<int32_t,3> origin;for(auto& c:origin){c=std::bit_cast<int32_t>(flags.u32());if(c&7)throw std::invalid_argument("unaligned point empty-schema leaf origin");}
    if(previousOrigin && origin<=*previousOrigin)throw std::invalid_argument("point empty-schema leaf origins are not unique/sorted");previousOrigin=origin;
    for(uint64_t attr=0;attr<attributeCount;++attr){const auto bits=flags.byte();const auto stride=flags.u32();if((bits&~uint8_t(points::AttributeArray::HIDDEN|points::AttributeArray::CONSTANTSTRIDE)) || !(bits&points::AttributeArray::CONSTANTSTRIDE) || !stride)throw std::invalid_argument("invalid point empty-schema array flags/stride");hidden[size_t(attr)]|=uint8_t(bool(bits&points::AttributeArray::HIDDEN));if(!leaf)strides[size_t(attr)]=stride;else if(strides[size_t(attr)]!=stride)strides[size_t(attr)]=0;}
  }
  for(size_t attr=0;attr<attributeCount;++attr)if(result.schema.attributes[attr].anyHidden!=bool(hidden[attr]) || result.schema.attributes[attr].stride!=strides[attr])throw std::invalid_argument("point empty-schema per-leaf flags disagree with public schema");
  flags.end();result.present=true;return result;
}
long double schemaBytes(const PointAttributeSchema& schema) {
  long double bytes=4096;for(const auto& a:schema.attributes)bytes+=a.name.size()+a.valueType.size()+a.codec.size()+128;
  for(const auto& g:schema.groups)bytes+=g.size()+64;return bytes;
}
void archiveEmpty(const Loaded& source,Grid& target,const PointAttributeOptions& o,const ProcessingOptions& p) {
  for(const auto* key:ArchiveKeys)if(target[key])throw std::invalid_argument("reserved point empty-schema archive key already exists");
  const long double maximum=2*source.metadata+schemaBytes(source.schema)+source.leaves.size()*(12.0L+5*source.schema.attributes.size())+4096;
  if(maximum>UINT32_MAX)throw std::invalid_argument("point empty-schema archive exceeds native metadata byte range");
  budget(source.snapshot->encoded+5*source.decoded+12*source.metadata+8*maximum+Fixed,p,"point empty-schema serialization exceeds shared memory budget");
  const auto cap=size_t(maximum);DescriptorBuffer buffer(cap);std::ostream stream(&buffer);stream.exceptions(std::ios::badbit|std::ios::failbit);
  source.leaves.front()->attributeSet().descriptor().write(stream);stream.flush();
  Bytes flags(cap);flags.put(FlagsMagic);flags.u64(source.leaves.size());flags.u64(source.schema.attributes.size());
  for(const auto* leaf:source.leaves) {
    for(size_t axis=0;axis<3;++axis)flags.u32(uint32_t(leaf->origin()[axis]));
    for(const auto& a:source.schema.attributes){const auto& array=leaf->constAttributeArray(a.name);flags.byte(array.flags()&uint8_t(points::AttributeArray::HIDDEN|points::AttributeArray::CONSTANTSTRIDE));flags.u32(array.stride());}
  }
  Bytes manifest(cap);manifest.put(ManifestMagic);manifest.u32(12);manifest.u64(source.schema.attributes.size());manifest.u64(source.schema.groups.size());
  manifest.u64(source.leaves.size());manifest.u64(source.schema.pointCount);manifest.u64(source.stringBytes);manifest.u64(buffer.bytes.bytes.size());manifest.u64(checksum(buffer.bytes.bytes));manifest.u64(flags.bytes.size());manifest.u64(checksum(flags.bytes));
  for(const auto& a:source.schema.attributes){manifest.string(a.name);manifest.string(a.valueType);manifest.string(a.codec);manifest.u32(a.stride);manifest.byte(uint8_t(a.groupStorage)|(uint8_t(a.anyHidden)<<1)|(uint8_t(a.readable)<<2));}
  for(const auto& group:source.schema.groups)manifest.string(group);
  target.insertMeta(ArchiveKeys[0],openvdb::StringMetadata(manifest.bytes));target.insertMeta(ArchiveKeys[1],openvdb::StringMetadata(buffer.bytes.bytes));target.insertMeta(ArchiveKeys[2],openvdb::StringMetadata(flags.bytes));
  (void)o;
}
struct Predicate {
  std::array<std::vector<points::GroupHandle>,3> handles;
  Predicate(const Leaf& leaf,const PointGroupSelection& s){size_t i=0;for(const auto* names:{&s.anyOf,&s.allOf,&s.noneOf}){handles[i].reserve(names->size());for(const auto& n:*names)handles[i].push_back(leaf.groupHandle(n));++i;}}
  bool operator()(openvdb::Index n)const {
    bool any=handles[0].empty();for(const auto& h:handles[0])any|=h.get(n);if(!any)return false;
    for(const auto& h:handles[1])if(!h.get(n))return false;for(const auto& h:handles[2])if(h.get(n))return false;return true;
  }
};
long double validateSelection(const PointGroupSelection& selection,const PointAttributeSchema& schema,const PointAttributeOptions& o) {
  long double bytes=4096;size_t count=0;
  for(const auto* names:{&selection.anyOf,&selection.allOf,&selection.noneOf})for(const auto& n:*names) {
    if(++count>3*o.maxGroups)throw std::invalid_argument("point selection predicate exceeds group-name limit");name(n);
    if(std::find(schema.groups.begin(),schema.groups.end(),n)==schema.groups.end())throw std::invalid_argument("point selection group was not found: "+n);
    bytes+=4.0L*n.size()+512;
  }
  return bytes;
}
struct Selected {PointSelectionCounts counts;std::vector<uint8_t> keep;};
Selected select(const Loaded& source,const PointGroupSelection& selection,const PointAttributeOptions& o,const ProcessingOptions& p,bool mask) {
  const auto archive=inspectArchive(source,o,p);Selected result;result.counts.schema=archive.present?archive.schema:source.schema;result.counts.archivedSchema=archive.present;
  const auto reserve=validateSelection(selection,result.counts.schema,o);
  budget(source.snapshot->encoded+4*source.decoded+8*source.metadata+reserve+3.0L*source.schema.pointCount+Fixed,p,"point selection predicate/mask exceeds shared memory budget");
  result.counts.total=source.schema.pointCount;if(mask)result.keep.reserve(size_t(source.schema.pointCount));
  for(const auto* leaf:source.leaves) {
    Predicate predicate(*leaf,selection);
    for(auto point=leaf->beginIndexAll();point;++point) {
      const bool chosen=predicate(*point);if(mask)result.keep.push_back(uint8_t(chosen));
      result.counts.selected+=chosen;
      if(chosen){if(leaf->isValueOn(point.getCoord()))++result.counts.selectedActive;else ++result.counts.selectedInactive;}
    }
  }
  if(mask && result.keep.size()!=source.schema.pointCount)throw std::runtime_error("point selection iterator disagrees with validated offsets");return result;
}
struct Mapping {
  const std::vector<std::pair<openvdb::Index,openvdb::Index>>& data;size_t index=0;
  explicit operator bool()const{return index<data.size();}Mapping& operator++(){++index;return *this;}
  openvdb::Index sourceIndex()const{return data[index].first;}openvdb::Index targetIndex()const{return data[index].second;}
};
void compact(const Loaded& source,Grid& target,const std::vector<uint8_t>& selected,PointSelectionMode mode) {
  size_t offset=0;
  for(const auto* original:source.leaves) {
    auto* leaf=target.tree().probeLeaf(original->origin());std::vector<std::pair<openvdb::Index,openvdb::Index>> mapping;
    mapping.reserve(size_t(original->pointCount()));std::vector<Leaf::ValueType> offsets;offsets.reserve(Leaf::NUM_VALUES);
    openvdb::Index previous=0;
    for(openvdb::Index voxel=0;voxel<Leaf::NUM_VALUES;++voxel) {
      const auto end=original->getValue(voxel);
      for(auto n=previous;n<end;++n)if((selected[offset+n]!=0)==(mode==PointSelectionMode::KeepSelected))mapping.emplace_back(n,openvdb::Index(mapping.size()));
      offsets.push_back(Leaf::ValueType(mapping.size()));previous=end;
    }
    offset+=size_t(original->pointCount());if(mapping.size()==original->pointCount())continue;
    if(mapping.empty()){std::unique_ptr<Leaf> removed(target.tree().template stealNode<Leaf>(original->origin(),0,false));continue;}
    auto attrs=std::make_unique<points::AttributeSet>(original->attributeSet(),openvdb::Index(mapping.size()));
    for(size_t a=0;a<attrs->size();++a) {
      Mapping map{mapping};const auto& sourceArray=original->constAttributeArray(a);
      points::AttributeArray::Ptr expanded;
      if(sourceArray.isUniform() && sourceArray.stride()>1) {
        // Upstream raw transfer copies storageSize*stride bytes from a uniform
        // array's one-element allocation. Expand a budgeted native copy first.
        sourceArray.loadData();expanded=sourceArray.copy();expanded->expand();
      }
      // Numeric compaction compares decoded values, treating+0/-0 as equal and
      // potentially re-encoding opaque codecs. Keep exact copied native bytes.
      attrs->get(a)->copyValues(expanded?*expanded:sourceArray,map,false);
    }
    leaf->replaceAttributeSet(attrs.release());leaf->setOffsets(offsets,false);leaf->validateOffsets();
  }
}
long double expandedCompactionBytes(const Loaded& source,const std::vector<uint8_t>& selected,PointSelectionMode mode) {
  long double bytes=0;size_t offset=0;
  for(const auto* leaf:source.leaves) {
    uint64_t retained=0;for(size_t n=0;n<leaf->pointCount();++n)retained+=((selected[offset+n]!=0)==(mode==PointSelectionMode::KeepSelected));
    offset+=size_t(leaf->pointCount());if(!retained || retained==leaf->pointCount())continue;
    for(size_t a=0;a<leaf->attributeSet().size();++a) {
      const auto& array=leaf->constAttributeArray(a);
      // copyValues expands the destination even for uniform input. Loaded
      // uniform memory is therefore not a bound on prospective strided data.
      const auto elements=static_cast<long double>(leaf->pointCount())*array.stride();
      const auto strideBytes=static_cast<long double>(array.stride())*array.storageTypeSize();
      if(elements>std::numeric_limits<openvdb::Index>::max() || strideBytes>std::numeric_limits<openvdb::Index>::max())
        throw std::invalid_argument("point compaction strided elements/bytes exceed upstream Index32 multiplication range");
      bytes+=static_cast<long double>(retained)*strideBytes+512;
      if(array.isUniform() && array.stride()>1)bytes+=elements*array.storageTypeSize()+512;
    }
  }
  return bytes;
}
long double outputBytes(const Grid& grid,const PointAttributeOptions& o) {
  long double bytes=grid.memUsage()+4*metadataBytes(grid);
  for(auto leaf=grid.tree().cbeginLeaf();leaf;++leaf) {
    leaf->validateOffsets();if(leaf->attributeSet().size()>o.maxAttributes)throw std::runtime_error("point compaction exceeded attribute cap");
    bytes+=4*metadataBytes(leaf->attributeSet().descriptor().getMetadata());
    for(size_t n=0;n<leaf->attributeSet().size();++n)bytes+=leaf->constAttributeArray(n).memUsageIfLoaded();
  }
  return bytes;
}
using exact_statistics_detail::Moment;
template<class T>PointAttributeStatistics measure(const Loaded& source,const PointAttributeDescription& description,const PointGroupSelection& selection) {
  PointAttributeStatistics result;result.attribute=description;result.minimum=std::vector<T>{};result.maximum=std::vector<T>{};
  constexpr bool vector=std::is_same_v<T,std::array<float,3>> || std::is_same_v<T,std::array<double,3>>;
  result.componentCount=vector?3:1;std::array<Moment,3> moments;T minimum{},maximum{};
  for(const auto* leaf:source.leaves) {
    Predicate predicate(*leaf,selection);points::AttributeHandle<typename Native<T>::Type,points::NullCodec> handle(leaf->constAttributeArray(description.name));
    for(openvdb::Index n=0;n<leaf->pointCount();++n)if(predicate(n)) {
      const auto value=Native<T>::from(handle.get(n));finite(value);
      if(!result.hasValues){minimum=maximum=value;result.hasValues=true;}
      if constexpr(vector)for(size_t c=0;c<3;++c){minimum[c]=std::min(minimum[c],value[c]);maximum[c]=std::max(maximum[c],value[c]);moments[c].add(value[c]);}
      else{minimum=std::min(minimum,value);maximum=std::max(maximum,value);moments[0].add(value);}
      ++result.selectedCount;
    }
  }
  if(result.hasValues){result.minimum=std::vector<T>{minimum};result.maximum=std::vector<T>{maximum};for(size_t c=0;c<result.componentCount;++c){const auto [mean,deviation]=moments[c].finish(result.selectedCount);result.mean[c]=mean;result.populationStdDev[c]=deviation;}}
  return result;
}
#endif
} // namespace

PointSelectionCounts CountSelectedPoints(const std::filesystem::path& input,const PointGroupSelection& selection,const std::string& inputGrid,const ProcessingOptions& p,const PointAttributeOptions& o) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  validateOptions(p,o);initialize();tbb::task_arena arena(1);return arena.execute([&]{const auto source=load(input,inputGrid,p,o);return select(source,selection,o,p,false).counts;});
#else
  (void)input;(void)selection;(void)inputGrid;(void)p;(void)o;unavailable();
#endif
}
PointSelectionResult WriteSelectedPoints(const std::filesystem::path& input,const std::filesystem::path& output,const PointGroupSelection& selection,PointSelectionMode mode,const std::string& inputGrid,const ProcessingOptions& p,const PointAttributeOptions& o) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  validateOptions(p,o,true);if(mode!=PointSelectionMode::KeepSelected && mode!=PointSelectionMode::DeleteSelected)throw std::invalid_argument("unknown point selection mode");
  if(output.empty() || std::filesystem::exists(output) || (output.has_parent_path() && !std::filesystem::is_directory(output.parent_path())))throw std::invalid_argument("point selection output must be a new path in an existing directory");
  initialize();tbb::task_arena arena(1);return arena.execute([&] {
    const auto source=load(input,inputGrid,p,o);const auto chosen=select(source,selection,o,p,true);
    const uint64_t outputCount=mode==PointSelectionMode::KeepSelected?chosen.counts.selected:source.schema.pointCount-chosen.counts.selected;
    if(outputCount!=source.schema.pointCount)for(const auto* leaf:source.leaves)for(size_t n=0;n<leaf->attributeSet().size();++n)if(!leaf->constAttributeArray(n).hasConstantStride())throw std::invalid_argument("point deletion requires constant-stride attributes; dynamic-stride compaction is unsupported");
    const auto expanded=expandedCompactionBytes(source,chosen.keep,mode);
    budget(source.snapshot->encoded+5*source.decoded+12*source.metadata+4*expanded+64.0L*source.schema.pointCount+source.leaves.size()*4096.0L+Fixed,p,"point compaction clone/expanded strided attribute/mapping workspace exceeds shared memory budget");
    auto target=detail::DeepCopyGridExact(*source.grid);bool archived=chosen.counts.archivedSchema;
    if(outputCount==0 && !source.leaves.empty()){archiveEmpty(source,*target,o,p);target->tree().clear();archived=true;}
    else if(outputCount!=source.schema.pointCount)compact(source,*target,chosen.keep,mode);
    target->setName(p.gridName);target->removeMeta("container:point_count");target->insertMeta("container:point_count",openvdb::Int64Metadata(int64_t(outputCount)));
    budget(source.snapshot->encoded+source.decoded+4*source.metadata+3*outputBytes(*target,o)+64.0L*source.schema.pointCount+Fixed,p,"point selection output/writer exceeds shared memory budget");
    std::vector<const Leaf*> leaves;uint64_t actual=0;
    for(auto leaf=target->tree().cbeginLeaf();leaf;++leaf){leaves.push_back(&*leaf);actual+=leaf->pointCount();}
    if(actual!=outputCount || target->transform()!=source.grid->transform())throw std::runtime_error("native point compaction changed point count/transform unexpectedly");
    auto schema=archived?chosen.counts.schema:describe(*target,leaves,actual);schema.gridName=target->getName();schema.pointCount=actual;schema.leafCount=leaves.size();
    writeExact(output,*target,p);return PointSelectionResult{source.schema.pointCount,chosen.counts.selected,actual,std::move(schema),archived};
  });
#else
  (void)input;(void)output;(void)selection;(void)mode;(void)inputGrid;(void)p;(void)o;unavailable();
#endif
}
PointAttributeStatistics MeasurePointAttribute(const std::filesystem::path& input,const std::string& attribute,const PointGroupSelection& selection,const std::string& inputGrid,const ProcessingOptions& p,const PointAttributeOptions& o) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  validateOptions(p,o);name(attribute);if(attribute=="P")throw std::invalid_argument("P contains voxel-local offsets; world-position statistics require a separate spatial query");
  initialize();tbb::task_arena arena(1);return arena.execute([&] {
    const auto source=load(input,inputGrid,p,o);const auto selected=select(source,selection,o,p,false);
    const auto& schema=selected.counts.schema;const auto found=std::find_if(schema.attributes.begin(),schema.attributes.end(),[&](const auto& a){return a.name==attribute;});
    if(found==schema.attributes.end() || !found->readable || found->groupStorage || typeIndex({found->valueType,found->codec})==7)throw std::invalid_argument("point statistics require a supported numeric NullCodec stride-one attribute");
    budget(source.snapshot->encoded+4*source.decoded+8*source.metadata+512.0L*1024+Fixed,p,"point statistics exact accumulator workspace exceeds shared memory budget");
    switch(typeIndex({found->valueType,found->codec})) {
      case 0:return measure<uint8_t>(source,*found,selection);case 1:return measure<int32_t>(source,*found,selection);case 2:return measure<int64_t>(source,*found,selection);
      case 3:return measure<float>(source,*found,selection);case 4:return measure<double>(source,*found,selection);case 5:return measure<std::array<float,3>>(source,*found,selection);case 6:return measure<std::array<double,3>>(source,*found,selection);
      default:throw std::invalid_argument("unsupported point statistics type");
    }
  });
#else
  (void)input;(void)attribute;(void)selection;(void)inputGrid;(void)p;(void)o;unavailable();
#endif
}
} // namespace container::geometry::vdb

