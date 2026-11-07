#include "Container/geometry/OpenVdbRegionQueries.h"
#include <stdexcept>
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
#include "Container/geometry/OpenVdbGridSelection.h"
#include "OpenVdbFileUtils.h"
#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <fstream>
#include <openvdb/io/GridDescriptor.h>
#include <openvdb/tools/Count.h>
#include <set>
#include <streambuf>
#include <tbb/task_arena.h>
#include <type_traits>
#endif
namespace container::geometry::vdb {
namespace {
[[noreturn]] void unavailable(){throw std::runtime_error("OpenVDB active region queries are unavailable; configure with -DENABLE_OPENVDB=ON.");}
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
constexpr uint64_t Fixed=4ull*1024*1024;
void require(bool b,const char* s){if(!b)throw std::invalid_argument(s);}
uint64_t add(uint64_t a,uint64_t b){require(b<=UINT64_MAX-a,"region-query count/budget addition exceeds UInt64");return a+b;}
uint64_t mul(uint64_t a,uint64_t b){require(!a||b<=UINT64_MAX/a,"region-query count/budget product exceeds UInt64");return a*b;}
void bytes(uint64_t amount,const ActiveRegionOptions& o,const char* message){require(amount<=o.maxInputBytes,message);}
void validate(std::span<const IndexRegion> regions,const ActiveRegionOptions& o,const std::string& selector){
  require(o.maxInputBytes&&o.maxInputBytes<=(16ull<<30)&&o.maxStoredLeafVoxels&&o.maxNodes&&o.maxNodes<=1'000'000&&o.maxSourceActiveVoxels&&o.maxWork&&o.maxWork<=10'000'000'000ull&&o.maxGrids&&o.maxGrids<=4096&&o.maxQueries&&o.maxQueries<=4096&&regions.size()<=o.maxQueries&&o.maxMetadataBytes&&o.maxMetadataBytes<=(16ull<<20)&&o.maxRecordsPerQuery<=1'000'000&&o.maxTotalRecords<=1'000'000,"invalid active region query byte/node/work/catalog/metadata/query/record caps");
  require(selector.size()<=o.maxMetadataBytes,"region query selector exceeds metadata cap");
  for(const auto& r:regions)for(int a=0;a<3;++a)require(r.minimum[a]<=r.maximum[a],"active region query requires ordered inclusive Int32 bounds");
}
template<class F>auto visit(const openvdb::GridBase& g,F f)->decltype(f(static_cast<const openvdb::FloatGrid&>(g))){
#define TYPE(G) if(g.isType<openvdb::G>())return f(static_cast<const openvdb::G&>(g))
  TYPE(FloatGrid);TYPE(DoubleGrid);TYPE(BoolGrid);TYPE(MaskGrid);TYPE(Int32Grid);TYPE(Int64Grid);TYPE(Vec3IGrid);TYPE(Vec3SGrid);TYPE(Vec3DGrid);
#undef TYPE
  throw std::invalid_argument("active region queries require Float/Double/Bool/Mask/Int32/Int64/Vec3I/Vec3S/Vec3D; point/custom trees are unsupported");
}
bool supported(const openvdb::GridBase& g){return g.isType<openvdb::FloatGrid>()||g.isType<openvdb::DoubleGrid>()||g.isType<openvdb::BoolGrid>()||g.isType<openvdb::MaskGrid>()||g.isType<openvdb::Int32Grid>()||g.isType<openvdb::Int64Grid>()||g.isType<openvdb::Vec3IGrid>()||g.isType<openvdb::Vec3SGrid>()||g.isType<openvdb::Vec3DGrid>();}
template<class T>RegionNativeValue value(const T& v){
  if constexpr(std::is_same_v<T,float>)return RegionFloat32Bits{std::bit_cast<uint32_t>(v)};
  else if constexpr(std::is_same_v<T,double>)return RegionFloat64Bits{std::bit_cast<uint64_t>(v)};
  else if constexpr(std::is_same_v<T,openvdb::Vec3s>)return std::array<RegionFloat32Bits,3>{{{std::bit_cast<uint32_t>(v[0])},{std::bit_cast<uint32_t>(v[1])},{std::bit_cast<uint32_t>(v[2])}}};
  else if constexpr(std::is_same_v<T,openvdb::Vec3d>)return std::array<RegionFloat64Bits,3>{{{std::bit_cast<uint64_t>(v[0])},{std::bit_cast<uint64_t>(v[1])},{std::bit_cast<uint64_t>(v[2])}}};
  else if constexpr(std::is_same_v<T,openvdb::Vec3i>)return std::array<int32_t,3>{v[0],v[1],v[2]};
  else return v;
}
uint64_t volume(const IndexRegion& b){uint64_t n=1;for(int a=0;a<3;++a)n=mul(n,uint64_t(int64_t(b.maximum[a])-b.minimum[a]+1));return n;}
IndexRegion box(const openvdb::CoordBBox& b){IndexRegion out;for(int a=0;a<3;++a){out.minimum[a]=b.min()[a];out.maximum[a]=b.max()[a];}return out;}
bool intersection(const IndexRegion& a,const IndexRegion& b,IndexRegion& result){for(int i=0;i<3;++i){result.minimum[i]=std::max(a.minimum[i],b.minimum[i]);result.maximum[i]=std::min(a.maximum[i],b.maximum[i]);if(result.minimum[i]>result.maximum[i])return false;}return true;}
uint64_t metadataBound(const openvdb::MetaMap& map){uint64_t n=sizeof(map);for(auto it=map.beginMeta();it!=map.endMeta();++it){require(bool(it->second),"region query encountered null metadata");n=add(n,add(512,mul(4,add(add(it->first.size(),it->second->typeName().size()),add(it->second->size(),4)))));}return n;}
class FixedBuffer:public std::streambuf {public:explicit FixedBuffer(std::vector<uint8_t>& bytes){auto* p=reinterpret_cast<char*>(bytes.data());setp(p,p+bytes.size());}size_t used()const{return size_t(pptr()-pbase());}};
std::vector<RegionQueryMetadata> metadata(const openvdb::MetaMap& map,const ActiveRegionOptions& o){
  const auto bound=metadataBound(map);require(bound<=o.maxMetadataBytes,"region-query serialized metadata exceeds cap before allocation");std::vector<RegionQueryMetadata> result;result.reserve(map.metaCount());
  for(auto it=map.beginMeta();it!=map.endMeta();++it){RegionQueryMetadata m;m.name=it->first;m.type=it->second->typeName();const auto n=add(it->second->size(),4);require(n<=o.maxMetadataBytes,"region-query metadata record length exceeds cap before allocation");m.serializedValue.resize(size_t(n));FixedBuffer buffer(m.serializedValue);std::ostream stream(&buffer);stream.exceptions(std::ios::badbit|std::ios::failbit);it->second->write(stream);require(buffer.used()==n,"native metadata serialization size differs from its preflight");result.push_back(std::move(m));}return result;
}
struct Snapshot {
  std::filesystem::path directory,file;uint64_t encoded{};
  Snapshot(const std::filesystem::path& input,const ActiveRegionOptions& o){
    require(std::filesystem::is_regular_file(input),"active region query input must be a regular file");encoded=std::filesystem::file_size(input);bytes(add(Fixed,encoded),o,"encoded region source exceeds joint byte cap");require(mul(encoded,2)<=o.maxWork,"region source snapshot work exceeds cap before copy");
    static std::atomic_uint64_t serial{};for(int i=0;i<64;++i){auto candidate=std::filesystem::temp_directory_path()/("container-region-query-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+"-"+std::to_string(++serial));if(std::filesystem::create_directory(candidate)){directory=std::move(candidate);file=directory/"source.vdb";break;}}require(!file.empty(),"cannot reserve region source snapshot directory");
    try{std::ifstream from(input,std::ios::binary);std::ofstream to(file,std::ios::binary|std::ios::out|std::ios::noreplace);require(bool(from)&&bool(to),"cannot open region source snapshot");std::array<char,65536> buffer{};auto left=encoded;while(left){const auto n=std::streamsize(std::min<uint64_t>(left,buffer.size()));from.read(buffer.data(),n);require(from.gcount()==n,"region source changed length during snapshot");to.write(buffer.data(),n);left-=uint64_t(n);}to.close();require(bool(to)&&!from.bad()&&from.peek()==std::char_traits<char>::eof(),"region source snapshot failed or changed length");}catch(...){cleanup();throw;}
  }
  void cleanup()noexcept{std::error_code e;std::filesystem::remove(file,e);std::filesystem::remove(directory,e);}
  ~Snapshot(){cleanup();}
};
class MetadataArchive:public openvdb::io::Archive {
public:
  void begin(std::istream& in,openvdb::io::StreamMetadata::Ptr& metadata){readHeader(in);require(inputHasGridOffsets()&&fileVersion()>=openvdb::OPENVDB_FILE_VERSION_GRID_INSTANCING&&fileVersion()<=openvdb::OPENVDB_FILE_VERSION,"region queries require a supported seekable metadata-readable VDB");openvdb::io::setStreamMetadataPtr(in,metadata,false);setFormatVersion(in);setLibraryVersion(in);setDataCompression(in);}
  int32_t count(std::istream& in){return readGridCount(in);}
  openvdb::GridBase::Ptr next(std::istream& in,openvdb::io::GridDescriptor& d,uint64_t fileBytes,int64_t& mapBegin,int64_t& mapEnd){
    auto g=d.read(in);const auto after=int64_t(in.tellg()),start=d.getGridPos(),end=d.getEndPos(),blocks=d.getBlockPos();require(bool(g)&&start>=after&&end>start&&uint64_t(end)<=fileBytes&&(d.isInstance()||(blocks>=start&&blocks<=end)),"region query native descriptor offsets exceed encoded ranges");d.seekToGrid(in);readGridCompression(in);g->readMeta(in);mapBegin=int64_t(in.tellg());g->readTransform(in);mapEnd=int64_t(in.tellg());require(mapEnd>=mapBegin&&mapEnd<=(d.isInstance()?end:blocks),"region query transform crosses native payload boundary");if(fileVersion()<openvdb::OPENVDB_FILE_VERSION_NO_GRIDMAP&&g->getName().empty())g->setName(d.gridName());d.seekToEnd(in);return g;
  }
};
struct Loaded {openvdb::GridBase::Ptr grid,header;openvdb::MetaMap::Ptr fileMetadata;std::string descriptor;uint64_t meta{},catalog{},full{},slots{},nodes{},reportBytes{},reportRecords{},work{};int64_t mapBegin{},mapEnd{};};
Loaded load(const Snapshot& s,std::span<const IndexRegion> regions,const ActiveRegionOptions& o,const std::string& selector,ActiveRegionBatch& result){
  detail::RequireSeekableFile(s.file);require(openvdb::io::Archive::isDelayedLoadingEnabled(),"bounded region queries require delayed loading; unset OPENVDB_DISABLE_DELAYED_LOAD");
#ifndef OPENVDB_USE_DELAYED_LOADING
  throw std::invalid_argument("bounded region queries require native delayed-loading support");
#else
  Loaded r;r.reportRecords=std::min(o.maxTotalRecords,mul(o.maxRecordsPerQuery,regions.size()));r.reportBytes=add(Fixed,add(mul(2*sizeof(ActiveRegionRecord),r.reportRecords),mul(regions.size(),sizeof(ActiveRegionResult)+sizeof(IndexRegion)+256)));
  bytes(add(r.reportBytes,s.encoded),o,"region-query bounded report exceeds joint byte cap before load");
  auto sm=std::make_shared<openvdb::io::StreamMetadata>();sm->setSeekable(true);std::ifstream stream(s.file,std::ios::binary);stream.exceptions(std::ios::badbit|std::ios::failbit);MetadataArchive archive;archive.begin(stream,sm);r.fileMetadata=std::make_shared<openvdb::MetaMap>();r.fileMetadata->readMeta(stream);r.meta=metadataBound(*r.fileMetadata);r.catalog=add(Fixed,s.encoded);
  auto check=[&]{require(r.meta<=o.maxMetadataBytes,"region query aggregate file/scanned metadata exceeds cap");bytes(add(r.reportBytes,add(r.catalog,mul(r.meta,16))),o,"region-query catalog/report exceeds joint byte cap");};check();
  const int32_t count=archive.count(stream);require(count>=0&&uint32_t(count)<=o.maxGrids,"region-query scanned catalog exceeds maxGrids");std::set<std::string> descriptors;
  for(int32_t i=0;i<count;++i){openvdb::io::GridDescriptor d;int64_t begin{},end{};auto g=archive.next(stream,d,s.encoded,begin,end);const auto descriptor=openvdb::io::GridDescriptor::nameAsString(d.uniqueName());require(descriptors.insert(descriptor).second,"region-query descriptor names collide; rename ambiguous literal bracket-suffixed grids");r.catalog=add(r.catalog,add(mul(descriptor.size(),4),256));r.meta=add(r.meta,add(metadataBound(*g),uint64_t(end-begin)));check();if(!GridSelectionMatches(selector,descriptor,g->getName()))continue;if(!supported(*g)){require(selector.empty(),"explicit region-query selector has an unsupported point/custom native type");continue;}require(!r.header,"region-query grid selector is ambiguous; use descriptor:<exact descriptor>");const auto memory=g->getMetadata<openvdb::Int64Metadata>(openvdb::GridBase::META_FILE_MEM_BYTES);require(memory&&memory->value()>=0,"region-query source requires valid recorded memory statistics");bytes(add(r.reportBytes,add(r.catalog,add(mul(r.meta,16),mul(uint64_t(memory->value()),4)))),o,"recorded region source/report exceeds joint byte cap before loading");r.header=std::move(g);r.descriptor=descriptor;r.mapBegin=begin;r.mapEnd=end;}
  require(bool(r.header),"region-query found no matching supported grid");stream.close();
  openvdb::io::File file(s.file.string());file.setCopyMaxBytes(0);file.open(true);require(file.getUniqueTag()==archive.getUniqueTag(),"region-query immutable snapshot archive changed");r.grid=file.readGrid(r.descriptor);r.grid->clearMetadata();r.grid->insertMeta(*r.header);r.grid->setTransform(detail::CopyTransformExact(r.header->transform()));
  r.full=visit(*r.grid,[](const auto& g){return uint64_t(openvdb::tools::memUsageIfLoaded(g.tree(),false));});
  result.nativeBytes=r.full;result.workspaceUpperBound=add(r.reportBytes,add(r.catalog,add(s.encoded,add(mul(r.meta,16),mul(r.full,4)))));bytes(result.workspaceUpperBound,o,"actual full typed region source/report exceeds byte cap BEFORE paging");
  visit(*r.grid,[&](const auto& g){using Tree=typename std::decay_t<decltype(g)>::TreeType;using Upper=typename Tree::RootNodeType::ChildNodeType;using Lower=typename Upper::ChildNodeType;const auto nodes=g.tree().nodeCount();result.sourceLeafNodes=nodes[0];result.sourceLowerNodes=nodes[1];result.sourceUpperNodes=nodes[2];result.sourceRootEntries=g.tree().root().getTableSize();result.storedLeafVoxels=mul(result.sourceLeafNodes,512);result.storedInternalSlots=add(mul(result.sourceLowerNodes,Lower::NUM_VALUES),mul(result.sourceUpperNodes,Upper::NUM_VALUES));r.nodes=add(1,add(result.sourceRootEntries,add(result.sourceLeafNodes,add(result.sourceLowerNodes,result.sourceUpperNodes))));r.slots=add(result.sourceRootEntries,add(result.storedLeafVoxels,result.storedInternalSlots));return 0;});
  result.nodeAndRootEntryCount=r.nodes;require(r.nodes<=o.maxNodes&&result.storedLeafVoxels<=o.maxStoredLeafVoxels,"region source nodes/root entries/stored leaf slots exceed caps BEFORE paging");
  r.work=add(mul(s.encoded,2),add(mul(r.meta,16),add(r.full,add(mul(mul(r.slots,128),add(regions.size(),1)),add(mul(r.reportRecords,128),add(mul(regions.size(),512),65536))))));require(r.work<=o.maxWork,"region census/query/paging/catalog/report work exceeds cap BEFORE paging/traversal");result.workUpperBound=r.work;
  r.grid->readNonresidentBuffers();file.close();
  result.descriptorName=r.descriptor;result.gridName=r.header->getName();result.valueType=r.header->valueType();result.gridClass=openvdb::GridBase::gridClassToString(r.header->getGridClass());result.vectorType=openvdb::GridBase::vecTypeToString(r.header->getVectorType());result.inWorldSpace=r.header->isInWorldSpace();result.affine=r.header->transform().isLinear();result.transformType=r.header->transform().mapType();
  if(result.affine){std::array<double,16> m{0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1};const auto map=r.header->transform().baseMap();const auto origin=map->applyMap(openvdb::Vec3d(0));for(int a=0;a<3;++a){m[4*a+3]=origin[a];for(int b=0;b<3;++b){openvdb::Vec3d axis(0);axis[b]=1;m[4*a+b]=map->applyJacobian(axis)[a];}}if(std::all_of(m.begin(),m.end(),[](double x){return std::isfinite(x);}))result.indexToWorld=m;}
  require(uint64_t(r.mapEnd-r.mapBegin)<=o.maxMetadataBytes,"region-query transform bytes exceed cap before allocation");result.serializedTransform.resize(size_t(r.mapEnd-r.mapBegin));std::ifstream raw(s.file,std::ios::binary);raw.seekg(r.mapBegin);raw.read(reinterpret_cast<char*>(result.serializedTransform.data()),std::streamsize(result.serializedTransform.size()));require(bool(raw),"region-query source transform bytes are truncated");result.gridMetadata=metadata(*r.header,o);result.fileMetadata=metadata(*r.fileMetadata,o);return r;
#endif
}
template<class Grid>void query(const Grid& g,std::span<const IndexRegion> regions,const ActiveRegionOptions& o,ActiveRegionBatch& result){
  if constexpr(std::is_same_v<Grid,openvdb::BoolGrid>)result.nativeType=RegionValueType::Boolean;
  else if constexpr(std::is_same_v<Grid,openvdb::MaskGrid>)result.nativeType=RegionValueType::Mask;
  else if constexpr(std::is_same_v<Grid,openvdb::Int32Grid>)result.nativeType=RegionValueType::Int32;
  else if constexpr(std::is_same_v<Grid,openvdb::Int64Grid>)result.nativeType=RegionValueType::Int64;
  else if constexpr(std::is_same_v<Grid,openvdb::DoubleGrid>)result.nativeType=RegionValueType::Double;
  else if constexpr(std::is_same_v<Grid,openvdb::Vec3IGrid>)result.nativeType=RegionValueType::Vec3I;
  else if constexpr(std::is_same_v<Grid,openvdb::Vec3SGrid>)result.nativeType=RegionValueType::Vec3S;
  else if constexpr(std::is_same_v<Grid,openvdb::Vec3DGrid>)result.nativeType=RegionValueType::Vec3D;
  else result.nativeType=RegionValueType::Float;
  result.background=value(g.background());result.queries.resize(regions.size());
  for(size_t i=0;i<regions.size();++i)result.queries[i].region=regions[i];
  for(auto it=g.tree().cbeginValueOn();it;++it){const auto source=box(it.getBoundingBox());const auto count=volume(source);result.sourceActiveRecords=add(result.sourceActiveRecords,1);result.sourceActiveVoxels=add(result.sourceActiveVoxels,count);require(result.sourceActiveVoxels<=o.maxSourceActiveVoxels,"region-query exact source active voxel census exceeds cap");if(!it.isVoxelValue()){result.sourceActiveTiles=add(result.sourceActiveTiles,1);result.sourceActiveTileVoxels=add(result.sourceActiveTileVoxels,count);}for(auto& q:result.queries){IndexRegion clipped;if(!intersection(source,q.region,clipped))continue;q.matchedRecordCount=add(q.matchedRecordCount,1);q.matchedVoxelCount=add(q.matchedVoxelCount,volume(clipped));}}
  std::vector<uint64_t> quotas;quotas.reserve(result.queries.size());uint64_t remaining=o.maxTotalRecords;for(auto& q:result.queries){const auto n=std::min({q.matchedRecordCount,o.maxRecordsPerQuery,remaining});q.records.reserve(size_t(n));quotas.push_back(n);remaining-=n;}
  if(!quotas.empty())for(auto it=g.tree().cbeginValueOn();it;++it){const auto source=box(it.getBoundingBox());for(size_t i=0;i<result.queries.size();++i){auto& q=result.queries[i];if(q.records.size()>=quotas[i])continue;IndexRegion clipped;if(!intersection(source,q.region,clipped))continue;const auto n=volume(clipped);q.records.push_back({source,clipped,uint32_t(it.getLevel()),!it.isVoxelValue(),n,value(*it)});q.returnedVoxelCount=add(q.returnedVoxelCount,n);}}
  for(auto& q:result.queries){q.omittedRecordCount=q.matchedRecordCount-q.records.size();q.omittedVoxelCount=q.matchedVoxelCount-q.returnedVoxelCount;q.recordsTruncated=q.omittedRecordCount!=0;}
}
#endif
} // namespace
ActiveRegionBatch QueryActiveRegions(const std::filesystem::path& input,std::span<const IndexRegion> regions,const ActiveRegionOptions& options,const std::string& selector){
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  validate(regions,options,selector);openvdb::initialize();tbb::task_arena arena(1);return arena.execute([&]{Snapshot source(input,options);ActiveRegionBatch result;const auto loaded=load(source,regions,options,selector,result);visit(*loaded.grid,[&](const auto& g){query(g,regions,options,result);return 0;});result.diagnostics={"Active native values are queried by index-box intersection; false/zero/nonfinite values remain hits when active, while inactive continuations never hit.","Count coverage is complete even when deterministic native-order record prefixes truncate. Query order consumes the aggregate record quota; overlapping queries count independently.","Float wrappers preserve native bits and vector components are raw; geometry, field semantics and map validity are not certified.","Full typed paging, stored nodes/root entries, source census, every query, metadata and bounded reports share logical caps; native eager metadata/topology decoding is not an allocator/RSS sandbox."};return result;});
#else
  (void)input;(void)regions;(void)options;(void)selector;unavailable();
#endif
}
} // namespace container::geometry::vdb
