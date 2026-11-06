#include "Container/geometry/OpenVdbValueComponents.h"
#include <stdexcept>
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
#include "Container/geometry/OpenVdbGridSelection.h"
#include "OpenVdbFileUtils.h"
#include "OpenVdbMetadataCatalog.h"
#include "OpenVdbProvenance.h"
#include <algorithm>
#include <charconv>
#include <fstream>
#include <openvdb/tools/Count.h>
#include <set>
#include <tbb/task_arena.h>
#include <type_traits>
#endif
namespace container::geometry::vdb {
namespace {
[[noreturn]] void unavailable(){throw std::runtime_error("OpenVDB value-preserving active components are unavailable; configure with -DENABLE_OPENVDB=ON.");}
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
constexpr uint64_t Fixed=4ull*1024*1024;
void require(bool condition,const char* message){if(!condition)throw std::invalid_argument(message);}
uint64_t add(uint64_t a,uint64_t b){require(b<=UINT64_MAX-a,"value-component count/budget addition exceeds UInt64");return a+b;}
uint64_t mul(uint64_t a,uint64_t b){require(!a||b<=UINT64_MAX/a,"value-component count/budget product exceeds UInt64");return a*b;}
void bytes(uint64_t n,const ActiveValueComponentsOptions& o,const char* message){require(n<=o.maxInputBytes,message);}
void work(uint64_t n,const ActiveValueComponentsOptions& o,const char* message){require(n<=o.maxWork,message);}
void validate(const ActiveValueComponentsOptions& o,const std::string& selector){
  require(o.maxComponents&&o.maxComponents<=64&&o.maxGrids&&o.maxGrids<=4096&&o.maxActiveVoxels&&o.maxStoredLeafVoxels&&o.maxAggregateOutputLeafVoxels&&o.maxNodes&&o.maxNodes<=1'000'000&&o.maxWork&&o.maxWork<=10'000'000'000ull&&o.maxInputBytes&&o.maxInputBytes<=(16ull<<30)&&o.maxOutputBytes&&o.maxOutputBytes<=(16ull<<30)&&o.maxMetadataBytes&&o.maxMetadataBytes<=(16ull<<20),"invalid value-component component/catalog/active/leaf/node/work/byte/metadata limits");
  require(!o.gridNamePrefix.empty()&&o.gridNamePrefix.size()<=128&&o.gridNamePrefix.find('\0')==std::string::npos,"value-component name prefix requires1..128 native bytes without NUL");
  require(selector.size()<=o.maxMetadataBytes,"value-component grid selector exceeds metadata cap");
}
uint64_t metadata(const openvdb::MetaMap& m){uint64_t n=sizeof(m);for(auto i=m.beginMeta();i!=m.endMeta();++i){require(bool(i->second),"null value-component metadata");n=add(n,add(512,mul(4,add(add(i->first.size(),i->second->typeName().size()),i->second->size()))));}return n;}
template<class F>auto visit(const openvdb::GridBase& g,F f)->decltype(f(static_cast<const openvdb::FloatGrid&>(g))){
#define G(T) if(g.isType<openvdb::T>())return f(static_cast<const openvdb::T&>(g))
  G(FloatGrid);G(DoubleGrid);G(BoolGrid);G(MaskGrid);G(Int32Grid);G(Int64Grid);G(Vec3IGrid);G(Vec3SGrid);G(Vec3DGrid);
#undef G
  throw std::invalid_argument("value components require Float/Double/Bool/Mask/Int32/Int64/Vec3I/Vec3S/Vec3D; point/custom trees are unsupported");
}
bool supported(const openvdb::GridBase& g){return g.isType<openvdb::FloatGrid>()||g.isType<openvdb::DoubleGrid>()||g.isType<openvdb::BoolGrid>()||g.isType<openvdb::MaskGrid>()||g.isType<openvdb::Int32Grid>()||g.isType<openvdb::Int64Grid>()||g.isType<openvdb::Vec3IGrid>()||g.isType<openvdb::Vec3SGrid>()||g.isType<openvdb::Vec3DGrid>();}
struct Snapshot {
  std::filesystem::path directory,file,result;uint64_t encoded{};
  Snapshot(const std::filesystem::path& input,const ActiveValueComponentsOptions& o){
    require(std::filesystem::is_regular_file(input),"value-component input must be a regular file");encoded=std::filesystem::file_size(input);bytes(add(Fixed,mul(encoded,3)),o,"encoded value-component source exceeds joint byte cap");work(mul(encoded,2),o,"value-component snapshot work exceeds cap before copy");directory=detail::CreateAsciiVdbTemporaryDirectory();file=directory/"source.vdb";result=directory/"result.vdb";
    try{std::ifstream source(input,std::ios::binary);std::ofstream target(file,std::ios::binary|std::ios::out|std::ios::noreplace);require(bool(source)&&bool(target),"cannot open value-component snapshot");std::array<char,65536> buffer{};auto left=encoded;while(left){const auto n=std::streamsize(std::min<uint64_t>(left,buffer.size()));source.read(buffer.data(),n);require(source.gcount()==n,"value-component source changed length during snapshot");target.write(buffer.data(),n);left-=uint64_t(n);}target.close();require(bool(target)&&!source.bad()&&source.peek()==std::char_traits<char>::eof(),"value-component snapshot failed or source changed length");}catch(...){cleanup();throw;}
  }
  void cleanup()noexcept{std::error_code e;std::filesystem::remove(result,e);std::filesystem::remove(file,e);std::filesystem::remove(directory,e);}
  ~Snapshot(){cleanup();}
};
struct Shape {
  uint64_t leaf{},lower{},upper{},root{};
  uint64_t leaves()const{return mul(leaf,512);}
  uint64_t nodes()const{return add(1,add(root,add(leaf,add(lower,upper))));}
  uint64_t slots()const{return add(root,add(leaves(),add(mul(lower,4096),mul(upper,32768))));}
};
template<class Tree>Shape shape(const Tree& tree){const auto n=tree.nodeCount();return {n[0],n[1],n[2],uint64_t(tree.root().getTableSize())};}
template<class Tree>uint64_t treeBytes(const Shape& s){
  using Leaf=typename Tree::LeafNodeType;using Upper=typename Tree::RootNodeType::ChildNodeType;using Lower=typename Upper::ChildNodeType;
  const auto leaf=add(sizeof(Leaf),add(mul(512,sizeof(typename Tree::ValueType)),512));
  const auto lower=add(sizeof(Lower),add(mul(Lower::NUM_VALUES,sizeof(typename Lower::UnionType)),1024));
  const auto upper=add(sizeof(Upper),add(mul(Upper::NUM_VALUES,sizeof(typename Upper::UnionType)),8192));
  return add(8192,add(mul(s.root,256),add(mul(s.leaf,leaf),add(mul(s.lower,lower),mul(s.upper,upper)))));
}
struct Loaded {
  openvdb::GridBase::Ptr grid,header;openvdb::MetaMap::Ptr fileMetadata;
  uint64_t metadataBytes{},catalogBytes{},fullBytes{},sourceBytes{},work{},catalogCount{};Shape source;
};
Loaded load(const Snapshot& s,const ActiveValueComponentsOptions& o,const std::string& selector){
  detail::RequireSeekableFile(s.file);require(openvdb::io::Archive::isDelayedLoadingEnabled(),"bounded value components require delayed loading; unset OPENVDB_DISABLE_DELAYED_LOAD");
#ifndef OPENVDB_USE_DELAYED_LOADING
  throw std::invalid_argument("bounded value components require native delayed-loading support");
#else
  Loaded r;auto sm=std::make_shared<openvdb::io::StreamMetadata>();sm->setSeekable(true);std::ifstream stream(s.file,std::ios::binary);stream.exceptions(std::ios::badbit|std::ios::failbit);detail::ExactMetadataArchive archive;archive.begin(stream,sm);r.fileMetadata=std::make_shared<openvdb::MetaMap>();r.fileMetadata->readMeta(stream);r.metadataBytes=metadata(*r.fileMetadata);r.catalogBytes=add(Fixed,mul(s.encoded,3));
  const auto check=[&]{require(r.metadataBytes<=o.maxMetadataBytes,"value-component aggregate file/scanned metadata exceeds cap");bytes(add(r.catalogBytes,mul(r.metadataBytes,16)),o,"value-component catalog/metadata exceeds joint byte cap");};check();
  const int32_t count=archive.count(stream);require(count>=0&&uint32_t(count)<=o.maxGrids,"value-component scanned catalog exceeds maxGrids");r.catalogCount=uint32_t(count);std::set<std::string> descriptors;std::string selected;
  for(int32_t i=0;i<count;++i){openvdb::io::GridDescriptor descriptor;auto g=archive.next(stream,descriptor,s.encoded);const auto name=openvdb::io::GridDescriptor::nameAsString(descriptor.uniqueName());require(descriptors.insert(name).second,"value-component descriptors collide; rename literal bracket-suffixed grids");r.catalogBytes=add(r.catalogBytes,add(mul(name.size(),4),256));const auto headerEnd=descriptor.isInstance()?descriptor.getEndPos():descriptor.getBlockPos();const auto headerBytes=uint64_t(headerEnd-descriptor.getGridPos());r.metadataBytes=add(r.metadataBytes,add(metadata(*g),headerBytes));check();if(!GridSelectionMatches(selector,name,g->getName()))continue;if(!supported(*g)){require(selector.empty(),"explicit value-component selector has unsupported point/custom type");continue;}require(!r.header,"value-component grid selector is ambiguous; use descriptor:<exact descriptor>");r.header=std::move(g);selected=name;}
  require(bool(r.header),"value components found no matching supported grid");const auto memory=r.header->getMetadata<openvdb::Int64Metadata>(openvdb::GridBase::META_FILE_MEM_BYTES);require(memory&&memory->value()>=0,"value components require valid recorded native memory statistics");bytes(add(r.catalogBytes,add(mul(r.metadataBytes,16),mul(uint64_t(memory->value()),4))),o,"recorded value-component native source exceeds joint cap before load");stream.close();
  openvdb::io::File file(s.file.string());file.setCopyMaxBytes(0);file.open(true);require(file.getUniqueTag()==archive.getUniqueTag(),"value-component private snapshot archive changed");r.grid=file.readGrid(selected);r.grid->clearMetadata();r.grid->insertMeta(*r.header);r.grid->setTransform(detail::CopyTransformExact(r.header->transform()));
  visit(*r.grid,[&](const auto& g){r.fullBytes=uint64_t(openvdb::tools::memUsageIfLoaded(g.tree(),false));r.source=shape(g.tree());r.sourceBytes=std::max(r.fullBytes,treeBytes<typename std::decay_t<decltype(g)>::TreeType>(r.source));return 0;});
  bytes(add(r.catalogBytes,add(mul(r.metadataBytes,16),mul(r.sourceBytes,4))),o,"full typed value-component source exceeds joint byte cap BEFORE paging");require(r.source.leaves()<=o.maxStoredLeafVoxels&&add(r.source.nodes(),r.catalogCount)<=o.maxNodes,"value-component source stored leaf slots/nodes exceed caps BEFORE paging");
  r.work=add(mul(s.encoded,2),add(mul(r.metadataBytes,16),add(r.sourceBytes,mul(r.source.slots(),128))));work(r.work,o,"value-component full source validation/paging work exceeds cap BEFORE paging");r.grid->readNonresidentBuffers();file.close();return r;
#endif
}
uint64_t volume(const openvdb::CoordBBox& b){uint64_t n=1;for(int a=0;a<3;++a)n=mul(n,uint64_t(int64_t(b.max()[a])-b.min()[a]+1));return n;}
uint64_t logBound(uint64_t n){uint64_t bits=0;while(n){++bits;n>>=1;}return bits;}
std::string name(const std::string& prefix,uint32_t index){std::array<char,16> digits;const auto r=std::to_chars(digits.data(),digits.data()+digits.size(),index);const auto n=size_t(r.ptr-digits.data());return prefix+"_"+std::string(6-n,'0')+std::string(digits.data(),n);}
template<class Grid>class ExactGrid final:public Grid {
public:ExactGrid(typename Grid::TreePtrType tree,const Grid& source):Grid(tree){this->insertMeta(source);this->setTransform(detail::CopyTransformExact(source.transform()));this->setSaveFloatAsHalf(false);}
  openvdb::GridBase::ConstPtr copyGrid()const override{return this->copyReplacingMetadataAndTransform(*this,detail::CopyTransformExact(this->transform()));}
  openvdb::GridBase::Ptr copyGrid()override{return openvdb::ConstPtrCast<openvdb::GridBase>(static_cast<const ExactGrid&>(*this).copyGrid());}
};
void publish(const std::filesystem::path& source,const std::filesystem::path& output){bool owned=false;try{std::ifstream from(source,std::ios::binary);std::ofstream to(output,std::ios::binary|std::ios::out|std::ios::noreplace);require(bool(to),"cannot exclusively create value-component output");owned=true;require(bool(from),"cannot read completed value-component output");from.exceptions(std::ios::badbit);to.exceptions(std::ios::badbit|std::ios::failbit);std::array<char,65536> buffer{};while(from){from.read(buffer.data(),std::streamsize(buffer.size()));if(from.gcount())to.write(buffer.data(),from.gcount());}require(from.eof(),"value-component publication source read failed");to.close();}catch(...){if(owned){std::error_code e;std::filesystem::remove(output,e);}throw;}}
template<class Grid>ActiveValueComponentsResult execute(const Grid& source,const Loaded& loaded,const Snapshot& snapshot,const std::filesystem::path& output,const ActiveValueComponentsOptions& o){
  using Tree=typename Grid::TreeType;using Labels=openvdb::Int32Tree;
  ActiveValueComponentsResult result;result.valueType=source.valueType();result.nativeSourceBytes=loaded.fullBytes;result.sourceStoredLeafVoxels=loaded.source.leaves();Shape expanded=loaded.source;
  for(auto it=source.tree().cbeginValueOn();it;++it){const auto n=volume(it.getBoundingBox());result.sourceActiveVoxels=add(result.sourceActiveVoxels,n);require(result.sourceActiveVoxels<=o.maxActiveVoxels,"exact value-component active census exceeds cap BEFORE expansion");if(!it.isVoxelValue()){result.sourceActiveTiles=add(result.sourceActiveTiles,1);const auto level=it.getLevel();require(level>=1&&level<=3,"unsupported value-component tile level");expanded.leaf=add(expanded.leaf,n/512);if(level>=2)expanded.lower=add(expanded.lower,n/(128ull*128*128));if(level==3)expanded.upper=add(expanded.upper,1);}}
  result.expandedStoredLeafVoxels=expanded.leaves();require(result.expandedStoredLeafVoxels<=o.maxStoredLeafVoxels,"value-component expanded leaf slots exceed cap BEFORE tile expansion");
  const auto expandedBytes=treeBytes<Tree>(expanded),labelBytes=treeBytes<Labels>(expanded);
  const auto fixed=add(loaded.catalogBytes,add(mul(loaded.metadataBytes,16),mul(loaded.sourceBytes,2)));
  const auto arrays=add(mul(result.sourceActiveVoxels,64),mul(o.maxComponents+1,1024));
  const auto working=add(fixed,add(expandedBytes,add(result.sourceActiveVoxels?labelBytes:0,arrays)));
  bytes(working,o,"value-component source/expanded/label/queue workspace exceeds cap BEFORE expansion/BFS");
  const auto workingNodes=add(loaded.catalogCount,add(mul(loaded.source.nodes(),2),mul(expanded.nodes(),result.sourceActiveVoxels?2:1)));
  require(workingNodes<=o.maxNodes,"joint value-component source/expanded/label nodes exceed cap BEFORE expansion/BFS");
  auto totalWork=add(loaded.work,add(mul(expanded.slots(),128),mul(result.sourceActiveVoxels,add(512,mul(32,logBound(result.sourceActiveVoxels))))));work(totalWork,o,"value-component expansion/label/sort/BFS work exceeds cap BEFORE allocation");
  auto tree=std::make_shared<Tree>(source.tree());tree->voxelizeActiveTiles(false);const auto actual=shape(*tree);require(actual.leaf==expanded.leaf&&actual.lower==expanded.lower&&actual.upper==expanded.upper&&actual.root==expanded.root,"native tile expansion differs from its preflight");
  std::vector<openvdb::Coord> coordinates,queue;std::unique_ptr<Labels> labels;
  std::vector<ActiveValueComponent> components;components.reserve(o.maxComponents+1);
  if(result.sourceActiveVoxels){coordinates.reserve(size_t(result.sourceActiveVoxels));queue.reserve(size_t(result.sourceActiveVoxels));for(auto it=tree->cbeginValueOn();it;++it){require(it.isVoxelValue(),"native active tiles remain after bounded expansion");require(coordinates.size()<result.sourceActiveVoxels,"expanded native active census exceeds preflight before array growth");coordinates.push_back(it.getCoord());}require(coordinates.size()==result.sourceActiveVoxels,"expanded native active census differs from source");
    const auto less=[](const openvdb::Coord& a,const openvdb::Coord& b){for(int axis=0;axis<3;++axis){if(a[axis]<b[axis])return true;if(a[axis]>b[axis])return false;}return false;};std::sort(coordinates.begin(),coordinates.end(),less);for(size_t i=1;i<coordinates.size();++i)require(coordinates[i]!=coordinates[i-1],"duplicate active value-component coordinate");
    labels=std::make_unique<Labels>(*tree,int32_t(0),openvdb::TopologyCopy());auto label=labels->getAccessor();const auto active=tree->getAccessor();
    for(const auto& seed:coordinates){if(label.getValue(seed))continue;require(components.size()<o.maxComponents,"value-component count exceeds maxComponents before any output clone");const int32_t id=int32_t(components.size()+1);ActiveValueComponent component;component.gridName=name(o.gridNamePrefix,uint32_t(id));component.activeBounds=IndexRegion{{seed[0],seed[1],seed[2]},{seed[0],seed[1],seed[2]}};queue.clear();queue.push_back(seed);label.setValueOnly(seed,id);
      for(size_t head=0;head<queue.size();++head){const auto c=queue[head];component.activeVoxelCount=add(component.activeVoxelCount,1);for(int axis=0;axis<3;++axis){component.activeBounds->minimum[axis]=std::min(component.activeBounds->minimum[axis],c[axis]);component.activeBounds->maximum[axis]=std::max(component.activeBounds->maximum[axis],c[axis]);for(int sign:{-1,1}){if((sign<0&&c[axis]==INT32_MIN)||(sign>0&&c[axis]==INT32_MAX))continue;auto next=c;next[axis]+=sign;if(active.isValueOn(next)&&!label.getValue(next)){require(queue.size()<result.sourceActiveVoxels,"value-component BFS escaped its finite domain before queue growth");label.setValueOnly(next,id);queue.push_back(next);}}}}
      component.storedLeafVoxels=result.expandedStoredLeafVoxels;components.push_back(std::move(component));
    }
  }
  result.activeComponentCount=components.size();result.emptySourceCopy=components.empty();if(components.empty())components.push_back({name(o.gridNamePrefix,0),0,result.expandedStoredLeafVoxels,{}});
  result.aggregateOutputLeafVoxels=mul(components.size(),result.expandedStoredLeafVoxels);require(result.aggregateOutputLeafVoxels<=o.maxAggregateOutputLeafVoxels,"aggregate value-component output leaf slots exceed cap BEFORE output clones");
  result.nodeUpperBound=add(workingNodes,mul(components.size(),expanded.nodes()));require(result.nodeUpperBound<=o.maxNodes,"aggregate value-component output nodes exceed cap BEFORE output clones");
  const auto outputs=mul(components.size(),expandedBytes);const auto outputMetadata=mul(components.size(),add(loaded.metadataBytes,4096));const auto encoded=add(Fixed,add(mul(outputs,2),mul(outputMetadata,4)));require(encoded<=o.maxOutputBytes,"conservative encoded value-component output exceeds cap BEFORE output clones");
  result.workspaceUpperBound=add(working,add(mul(outputs,2),add(mul(outputMetadata,16),encoded)));bytes(result.workspaceUpperBound,o,"aggregate value-component native/metadata/writer workspace exceeds cap BEFORE output clones");
  totalWork=add(totalWork,add(mul(mul(components.size(),expanded.slots()),128),add(mul(outputMetadata,16),mul(encoded,2))));work(totalWork,o,"aggregate value-component output/statistics/writer work exceeds cap BEFORE output clones");result.workUpperBound=totalWork;
  result.grids=components;result.diagnostics={"Each output is the entire SOURCE FIELD with one component's ACTIVITY. It is not an isolated solid, signed-distance repair, density/mass partition or cavity certificate.","Six-connected activity ignores values, including active false/zero/NaN/Inf. All non-Mask values/background/inactive continuations remain native-bit exact; Mask values equal activity.","Source classification/vector/world tags and exact maps are retained as labels. No coordinate/value/frame transform or pruning is performed. Canonical boundary-projection provenance is invalidated.","Tile expansion, finite native labeling/BFS, all component clones and writer/native+encoded scratch share checked logical caps. Native eager decoding is not an allocator/RSS/CPU sandbox."};if(result.emptySourceCopy)result.diagnostics.emplace_back("Empty source activity: one inactive source-field copy named with suffix_000000 retains all stored inactive values.");
  openvdb::GridPtrVec grids;grids.reserve(components.size());
  for(size_t i=0;i<components.size();++i){auto copy=std::make_shared<Tree>(*tree);if(labels){const int32_t id=int32_t(i+1);for(auto leaf=copy->beginLeaf();leaf;++leaf){const auto* nativeLabels=labels->probeConstLeaf(leaf->origin());require(nativeLabels!=nullptr,"value-component label/source leaf mismatch");leaf->setValuesOff();for(openvdb::Index n=0;n<512;++n)if(nativeLabels->getValue(n)==id)leaf->setValueOn(n);}}
    auto grid=std::make_shared<ExactGrid<Grid>>(copy,source);grid->setName(components[i].gridName);detail::RemoveBoundaryProjectionMetadata(*grid);require(grid->activeVoxelCount()==components[i].activeVoxelCount,"stored value-component activity differs from its counted component");grids.push_back(grid);
  }
  openvdb::io::File file(snapshot.result.string());file.setCompression(openvdb::io::COMPRESS_ZIP);file.write(grids,*loaded.fileMetadata);file.close();require(std::filesystem::file_size(snapshot.result)<=o.maxOutputBytes,"encoded value-component output exceeds file-size cap");
  publish(snapshot.result,output);static_assert(std::is_nothrow_move_constructible_v<ActiveValueComponentsResult>);return result;
}
#endif
} // namespace
ActiveValueComponentsResult WriteActiveValueComponents(const std::filesystem::path& input,const std::filesystem::path& output,const ActiveValueComponentsOptions& options,const std::string& selector){
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  validate(options,selector);require(!output.empty()&&!std::filesystem::exists(output),"value-component output must be a new file");const auto parent=output.has_parent_path()?output.parent_path():std::filesystem::path(".");require(std::filesystem::is_directory(parent),"value-component output parent must exist");openvdb::initialize();tbb::task_arena arena(1);return arena.execute([&]{Snapshot snapshot(input,options);const auto source=load(snapshot,options,selector);return visit(*source.grid,[&](const auto& grid){return execute(grid,source,snapshot,output,options);});});
#else
  (void)input;(void)output;(void)options;(void)selector;unavailable();
#endif
}
} // namespace container::geometry::vdb
