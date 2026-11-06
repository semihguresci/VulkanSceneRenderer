#pragma once

// Private per-translation-unit native point motion/repartition engine.
#include "Container/geometry/OpenVdbPointMotion.h"
#include <stdexcept>

#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
#include "OpenVdbPointAttributesInternal.h"
#include <openvdb/io/GridDescriptor.h>
#include <openvdb/points/PointMove.h>
#include <openvdb/tools/Count.h>
#include <tbb/task_arena.h>
#include <bit>
#include <cstring>
#include <map>
#include <sstream>
#include <type_traits>
#include <functional>
#endif

namespace container::geometry::vdb {
namespace {
[[noreturn]] void unavailable() {
  throw std::runtime_error("OpenVDB native point motion is unavailable; configure with -DENABLE_OPENVDB=ON.");
}
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
using namespace point_attribute_detail;
using V = openvdb::Vec3d;
using C = openvdb::Coord;
using Tree = Grid::TreeType;
using Upper = Tree::RootNodeType::ChildNodeType;
using Lower = Upper::ChildNodeType;
constexpr std::array<const char*,3> ArchiveKeys{
    "container:point_empty_manifest_v1", "container:point_empty_descriptor_v1",
    "container:point_empty_leaf_flags_v1"};

double spacing(double x) {
  return std::max(std::abs(std::nextafter(x, INFINITY)-x),
                  std::abs(x-std::nextafter(x,-INFINITY)));
}
struct Sum {
  double value=0, correction=0, magnitude=0;
  void add(double x) {
    const double next=value+x;
    correction+=std::abs(value)>=std::abs(x)?(value-next)+x:(x-next)+value;
    value=next; magnitude+=std::abs(x);
  }
  void product(double a,double b) {
    const double x=a*b;
    if(!std::isfinite(x) || (a!=0 && b!=0 && x==0))
      throw std::invalid_argument("point motion coordinate product overflows or underflows");
    add(x); add(std::fma(a,b,-x));
  }
  double get() const {
    const double x=value+correction;
    if(!std::isfinite(x)) throw std::invalid_argument("point motion coordinate sum is nonfinite");
    return x;
  }
  double uncertainty() const {
    // Neumaier additions and FMA product residuals leave a second-order
    // rounding bound, in addition to final Double rounding. Reject extreme
    // cancellation instead of treating a small final result as proof of accuracy.
    constexpr double eps=std::numeric_limits<double>::epsilon();
    return 64*eps*eps*magnitude+spacing(get());
  }
};
struct Affine {
  V origin;
  std::array<V,3> axes, inverseRows;
  double minimum=INFINITY, maximum=0, inverseNorm=0, condition=0;
  explicit Affine(const openvdb::math::Transform& t,bool uniform=false) {
    if(!t.isLinear()) throw std::invalid_argument("point motion requires affine maps");
    origin=t.indexToWorld(V(0));
    const auto m=t.baseMap();
    axes={m->applyJacobian({1,0,0}),m->applyJacobian({0,1,0}),m->applyJacobian({0,0,1})};
    auto unit=axes;
    double lengths[3];
    for(size_t a=0;a<3;++a) {
      lengths[a]=std::hypot(axes[a][0],axes[a][1],axes[a][2]);
      if(!std::isfinite(origin[a]) || !std::isfinite(lengths[a]) || lengths[a]<1e-6 || lengths[a]>1e6)
        throw std::invalid_argument("point motion affine axes must span [1e-6,1e6] world units");
      minimum=std::min(minimum,lengths[a]); maximum=std::max(maximum,lengths[a]); unit[a]/=lengths[a];
    }
    const double det=unit[0].dot(unit[1].cross(unit[2]));
    if(!std::isfinite(det) || std::abs(det)<1e-8)
      throw std::invalid_argument("point motion affine is singular or insufficiently conditioned");
    if(uniform) for(size_t a=0;a<3;++a) {
      if(std::abs(lengths[a]-lengths[0])>lengths[0]*1e-12)
        throw std::invalid_argument("point motion target axes must have uniform lengths");
      for(size_t b=a+1;b<3;++b) if(std::abs(unit[a].dot(unit[b]))>1e-12)
        throw std::invalid_argument("point motion target axes must be orthogonal");
    }
    inverseRows={unit[1].cross(unit[2])/(det*lengths[0]),
                 unit[2].cross(unit[0])/(det*lengths[1]),
                 unit[0].cross(unit[1])/(det*lengths[2])};
    double norm=0;
    for(size_t a=0;a<3;++a) {
      inverseNorm=std::max(inverseNorm,std::abs(inverseRows[a][0])+std::abs(inverseRows[a][1])+std::abs(inverseRows[a][2]));
      norm=std::max(norm,std::abs(axes[0][a])+std::abs(axes[1][a])+std::abs(axes[2][a]));
    }
    condition=norm*inverseNorm;
  }
  V world(const C& c,const openvdb::Vec3f& local,double requiredPrecision=0) const {
    V result;
    for(size_t a=0;a<3;++a) if(!std::isfinite(local[a]) || std::abs(local[a])>.500001f)
      throw std::invalid_argument("point motion P must have finite local offsets in [-.5,.5]");
    for(size_t row=0;row<3;++row) {
      Sum s;s.add(origin[row]);
      for(size_t a=0;a<3;++a) {s.product(axes[a][row],double(c[a]));s.product(axes[a][row],double(local[a]));}
      result[row]=s.get();
      if(s.uncertainty()>std::min(minimum*1e-4,requiredPrecision>0?requiredPrecision:INFINITY))
        throw std::invalid_argument("point motion world position loses source voxel precision; recenter the source");
    }
    return result;
  }
  V index(const V& world) const {
    V delta;
    double envelope=1;
    for(size_t a=0;a<3;++a) {
      Sum s;s.add(world[a]);s.add(-origin[a]);delta[a]=s.get();
      envelope=std::max(envelope,std::abs(world[a])+std::abs(origin[a]));
    }
    const long double error=128*std::numeric_limits<double>::epsilon()*condition*inverseNorm*envelope;
    if(!std::isfinite(error) || error>1e-4)
      throw std::invalid_argument("point motion target inverse/world arithmetic loses voxel precision; recenter or condition maps");
    V result;
    for(size_t a=0;a<3;++a) {
      Sum s;for(size_t b=0;b<3;++b)s.product(inverseRows[a][b],delta[b]);result[a]=s.get();
      if(std::abs(result[a])>double(INT32_MAX)-8192 || spacing(result[a])>1e-4)
        throw std::invalid_argument("point motion destination exceeds native coordinate/precision bounds");
    }
    return result;
  }
};
bool sameMap(const openvdb::math::Transform& a,const openvdb::math::Transform& b) {
  if(a.indexToWorld(V(0))!=b.indexToWorld(V(0)))return false;
  for(const V axis:{V(1,0,0),V(0,1,0),V(0,0,1)})
    if(a.baseMap()->applyJacobian(axis)!=b.baseMap()->applyJacobian(axis))return false;
  return true;
}
void matrix(const std::array<double,16>& m) {
  for(double x:m)if(!std::isfinite(x))throw std::invalid_argument("point motion matrix entries must be finite");
  if(m[12]!=0 || m[13]!=0 || m[14]!=0 || m[15]!=1)
    throw std::invalid_argument("point motion matrix last row must equal 0,0,0,1");
}
V deform(const V& p,const std::array<double,16>& m,double minimum) {
  V q;
  for(size_t a=0;a<3;++a) {
    Sum s;s.add(m[4*a+3]);for(size_t b=0;b<3;++b)s.product(m[4*a+b],p[b]);q[a]=s.get();
    if(!std::isfinite(s.uncertainty()) || s.uncertainty()>minimum*1e-4)
      throw std::invalid_argument("point motion deformed world coordinate loses target voxel precision");
    Sum displacement;displacement.add(m[4*a+3]);displacement.add(-p[a]);
    for(size_t b=0;b<3;++b)displacement.product(m[4*a+b],p[b]);
    if(displacement.get()!=0 && q[a]==p[a])
      throw std::invalid_argument("point motion nonzero deformation is unresolved in Double world coordinates; recenter the source");
  }
  return q;
}
bool identity(const std::array<double,16>& m) {
  constexpr std::array<double,16> id{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};return m==id;
}
openvdb::math::Transform::Ptr targetMap(const std::array<double,16>& m) {
  matrix(m);
  if(m[0]==m[5] && m[0]==m[10] && m[0]>0 &&
     m[1]==0 && m[2]==0 && m[4]==0 && m[6]==0 && m[8]==0 && m[9]==0) {
    const double h=m[0];
    if(h<1e-6 || h>1e6)throw std::invalid_argument("point motion target scale must be in [1e-6,1e6]");
    std::stringstream record(std::ios::in|std::ios::out|std::ios::binary);
    record.exceptions(std::ios::badbit|std::ios::failbit);
    openvdb::writeString(record,openvdb::math::UniformScaleTranslateMap::mapType());
    V(m[3],m[7],m[11]).write(record);
    for(double x:{h,h,1/h,1/(h*h),.5/h})V(x).write(record);
    record.seekg(0);openvdb::io::setCurrentVersion(record);
    auto result=std::make_shared<openvdb::math::Transform>();result->read(record);return result;
  }
  openvdb::math::Mat4d native;
  for(size_t a=0;a<4;++a)for(size_t b=0;b<4;++b)native[a][b]=m[4*b+a];
  try {return detail::MakeAffineTransformExact(native);}
  catch(const openvdb::ArithmeticError&) {
    throw std::invalid_argument("point motion target map cannot be represented by native AffineMap; tiny rotated maps exceed SDK determinant limits");
  }
}
struct Predicate {
  std::array<std::vector<points::GroupHandle>,3> handles;
  Predicate(const Leaf& leaf,const PointGroupSelection& s) {
    size_t i=0;for(const auto* names:{&s.anyOf,&s.allOf,&s.noneOf}) {
      for(const auto& n:*names)handles[i].push_back(leaf.groupHandle(n));++i;
    }
  }
  bool operator()(openvdb::Index n) const {
    bool any=handles[0].empty();for(const auto& h:handles[0])any|=h.get(n);
    if(!any)return false;for(const auto& h:handles[1])if(!h.get(n))return false;
    for(const auto& h:handles[2])if(h.get(n))return false;return true;
  }
};
void selection(const PointGroupSelection& s,const PointAttributeSchema& schema,const PointAttributeOptions& limits) {
  size_t terms=0;for(const auto* names:{&s.anyOf,&s.allOf,&s.noneOf})for(const auto& n:*names) {
    if(++terms>3*limits.maxGroups)throw std::invalid_argument("point motion group predicate exceeds count cap");
    name(n);if(std::find(schema.groups.begin(),schema.groups.end(),n)==schema.groups.end())
      throw std::invalid_argument("point motion selected group was not found: "+n);
  }
}
class MetadataArchive:public openvdb::io::Archive {
public:
  void begin(std::istream& s,openvdb::io::StreamMetadata::Ptr& m) {
    readHeader(s);
    if(!inputHasGridOffsets() || fileVersion()<openvdb::OPENVDB_FILE_VERSION_GRID_INSTANCING || fileVersion()>openvdb::OPENVDB_FILE_VERSION)
      throw std::invalid_argument("point motion requires supported seekable metadata-readable VDB");
    openvdb::io::setStreamMetadataPtr(s,m,false);setFormatVersion(s);setLibraryVersion(s);setDataCompression(s);
  }
  int32_t count(std::istream& s){return readGridCount(s);}
  openvdb::GridBase::Ptr next(std::istream& s,openvdb::io::GridDescriptor& d,uintmax_t bytes) {
    auto grid=d.read(s);const auto after=int64_t(s.tellg()),start=d.getGridPos(),end=d.getEndPos(),blocks=d.getBlockPos();
    if(!grid || start<after || end<=start || uintmax_t(end)>bytes || (!d.isInstance() && (blocks<start || blocks>end)))
      throw std::invalid_argument("point motion descriptor offsets exceed encoded file");
    d.seekToGrid(s);readGridCompression(s);grid->readMeta(s);grid->readTransform(s);
    if(fileVersion()<openvdb::OPENVDB_FILE_VERSION_NO_GRIDMAP && grid->getName().empty())grid->setName(d.gridName());
    if(int64_t(s.tellg())>(d.isInstance()?end:blocks))throw std::invalid_argument("point motion metadata crosses payload boundary");
    grid->removeMeta(openvdb::GridBase::META_FILE_DELAYED_LOAD);d.seekToEnd(s);return grid;
  }
};
struct Source {
  std::unique_ptr<Snapshot> snapshot;
  Grid::Ptr grid;
  openvdb::MetaMap::Ptr fileMetadata;
  std::vector<const Leaf*> leaves;
  PointAttributeSchema schema;
  long double resident=0,metadata=0,full=0,validationWork=0;
  bool archived=false;
  size_t catalogGridCount=0;
};
Source loadSource(const std::filesystem::path& input,const std::string& selector,const PointMotionOptions& o,const ProcessingOptions& p,const PointAttributeOptions& limits) {
  Source r;
  if(!std::filesystem::is_regular_file(input))throw std::invalid_argument("point motion input must be a regular file");
  r.validationWork=2.0L*std::filesystem::file_size(input);
  if(r.validationWork>o.maxWork)throw std::invalid_argument("point motion encoded snapshot copy exceeds maxWork before copy");
  r.snapshot=std::make_unique<Snapshot>(input,p);detail::RequireSeekableFile(r.snapshot->file);
  if(!openvdb::io::Archive::isDelayedLoadingEnabled())
    throw std::invalid_argument("bounded point motion requires delayed loading; unset OPENVDB_DISABLE_DELAYED_LOAD");
#ifndef OPENVDB_USE_DELAYED_LOADING
  throw std::invalid_argument("bounded point motion requires delayed-loading support");
#else
  auto sm=std::make_shared<openvdb::io::StreamMetadata>();sm->setSeekable(true);
  std::ifstream stream(r.snapshot->file,std::ios::binary);stream.exceptions(std::ios::badbit|std::ios::failbit);
  MetadataArchive archive;archive.begin(stream,sm);
  r.fileMetadata=std::make_shared<openvdb::MetaMap>();r.fileMetadata->readMeta(stream);
  r.metadata=metadataBytes(*r.fileMetadata);long double catalog=r.snapshot->encoded+Fixed,catalogWork=0;
  const auto check=[&] {
    if(r.metadata>o.maxMetadataBytes)throw std::invalid_argument("point motion aggregate file/grid/descriptor metadata exceeds maxMetadataBytes");
    if(r.validationWork+8*r.metadata+catalogWork>o.maxWork)throw std::invalid_argument("point motion encoded/catalog/metadata validation exceeds maxWork");
    budget(catalog+8*r.metadata,p,"point motion catalog/metadata exceeds memory budget");
  };check();
  const int32_t count=archive.count(stream);
  if(count<0 || uint32_t(count)>o.maxGrids)throw std::invalid_argument("point motion catalog exceeds maxGrids");
  r.catalogGridCount=size_t(count);
  std::set<std::string> descriptors;std::string chosen;openvdb::math::Transform::Ptr exact;
  bool found=false;
  for(int32_t i=0;i<count;++i) {
    openvdb::io::GridDescriptor d;auto grid=archive.next(stream,d,r.snapshot->encoded);
    const auto n=openvdb::io::GridDescriptor::nameAsString(d.uniqueName());catalog+=4.0L*n.size()+256;catalogWork+=4.0L*n.size()+256;
    if(!descriptors.insert(n).second)throw std::invalid_argument("point motion input has colliding grid descriptors");
    r.metadata+=metadataBytes(*grid);check();
    if(!grid->isType<Grid>() || !GridSelectionMatches(selector,n,grid->getName()))continue;
    if(found)throw std::invalid_argument("point motion grid is ambiguous; use descriptor:<exact name>");
    const auto memory=grid->getMetadata<openvdb::Int64Metadata>(openvdb::GridBase::META_FILE_MEM_BYTES),
               voxels=grid->getMetadata<openvdb::Int64Metadata>(openvdb::GridBase::META_FILE_VOXEL_COUNT);
    if(!memory || !voxels || memory->value()<0 || voxels->value()<0 || uint64_t(voxels->value())>p.maxVoxelCount)
      throw std::invalid_argument("point motion requires valid recorded memory/voxel statistics within budget");
    budget(catalog+8*r.metadata+memory->value(),p,"recorded point motion source exceeds memory budget");
    chosen=n;found=true;exact=detail::CopyTransformExact(grid->transform());
  }
  if(!found)throw std::invalid_argument("point motion found no matching native PointDataGrid");
  stream.close();openvdb::io::File file(r.snapshot->file.string());file.setCopyMaxBytes(0);file.open(true);
  if(file.getUniqueTag()!=archive.getUniqueTag())throw std::runtime_error("point motion private snapshot changed");
  r.grid=openvdb::gridPtrCast<Grid>(file.readGrid(chosen));
  if(!sameMap(r.grid->transform(),*exact))throw std::runtime_error("point motion native read changed exact source map");
  r.grid->setTransform(exact);
  const auto& tree=r.grid->tree();
  if(tree.hasActiveTiles() || tree.leafCount()>p.maxVoxelCount/512 || tree.leafCount()>UINT32_MAX || tree.activeVoxelCount()>p.maxVoxelCount)
    throw std::invalid_argument("point motion stored source topology exceeds voxel/leaf cap");
  const auto nodes=tree.nodeCount();
  r.validationWork+=tree.root().getTableSize()+nodes[0]*512.0L+nodes[1]*Lower::NUM_VALUES*1.0L+nodes[2]*Upper::NUM_VALUES*1.0L;
  if(r.validationWork>o.maxWork)throw std::invalid_argument("point motion source topology validation exceeds maxWork");
  const points::AttributeSet::Descriptor* first=nullptr;uint64_t pointCount=0;uintmax_t dictionary=0;
  for(auto leaf=tree.cbeginLeaf();leaf;++leaf) {
    const auto& descriptor=leaf->attributeSet().descriptor();
    if(!first)first=&descriptor;else if(descriptor!=*first)
      throw std::invalid_argument("point motion requires identical leaf descriptors/metadata");
    if(!leaf->hasAttribute("P") || descriptor.map().size()>limits.maxAttributes || descriptor.groupMap().size()>limits.maxGroups)
      throw std::invalid_argument("point motion schema exceeds attribute/group cap or lacks P");
    const auto& pos=leaf->constAttributeArray("P");
    if(!pos.hasValueType<openvdb::Vec3f>() || !pos.hasConstantStride() || pos.stride()!=1)
      throw std::invalid_argument("point motion P requires registered fixed stride-one Vec3f storage");
    if(pos.size()>limits.maxPoints-std::min<uint64_t>(pointCount,limits.maxPoints))
      throw std::invalid_argument("point motion source exceeds maxPoints");
    pointCount+=pos.size();
    for(size_t a=0;a<leaf->attributeSet().size();++a) {
      const auto& array=leaf->constAttributeArray(a);
      if(array.isTransient() || array.isStreaming())throw std::invalid_argument("point motion cannot preserve transient/streaming payloads");
      if(array.size()!=pos.size())throw std::invalid_argument("point motion attribute lengths disagree");
      if(array.hasConstantStride()) {
        const uint64_t count=uint64_t(array.size()),stride=uint64_t(array.stride());
        const auto width=array.storageTypeSize();
        if(!stride || !width || count>uint64_t(UINT32_MAX)/stride || stride>uint64_t(UINT32_MAX)/width)
          throw std::invalid_argument("point motion attribute size/stride exceeds native Index32 before full-memory estimation");
      }
    }
    for(const auto& [n,offset]:descriptor.groupMap()) {
      (void)offset;const auto index=leaf->attributeSet().groupIndex(n);
      if(index.first>=leaf->attributeSet().size())throw std::invalid_argument("point motion group backing is invalid");
      const auto& a=leaf->constAttributeArray(index.first);
      if(!points::isGroup(a) || !a.hasConstantStride() || a.stride()!=1)
        throw std::invalid_argument("point motion groups require fixed stride-one backing arrays");
    }
    r.metadata+=metadataBytes(descriptor.getMetadata());r.leaves.push_back(&*leaf);check();
  }
  if(first)for(auto i=first->getMetadata().beginMeta();i!=first->getMetadata().endMeta();++i)if(i->first.starts_with("string:")) {
    const auto text=std::dynamic_pointer_cast<openvdb::StringMetadata>(i->second);
    if(!text || text->value().size()>limits.maxStringBytes-dictionary)
      throw std::invalid_argument("point motion native string dictionary exceeds maxStringBytes or is invalid");
    dictionary+=text->value().size();
  }
  r.full=openvdb::tools::memUsageIfLoaded(tree,false);
  const auto terms=o.groups.anyOf.size()+o.groups.allOf.size()+o.groups.noneOf.size();
  r.validationWork+=8*r.metadata+catalogWork;
  r.validationWork+=pointCount*(128.0L+4*terms)+r.leaves.size()*limits.maxAttributes*8.0L;
  if(r.validationWork>o.maxWork)throw std::invalid_argument("point motion position/group validation exceeds maxWork");
  r.resident=catalog+12*r.metadata+4*r.full+pointCount*2048.0L+Fixed;
  budget(r.resident,p,"point motion full decoded source/planning workspace exceeds memory budget before materialization");
  r.grid->readNonresidentBuffers();
  for(const auto* leaf:r.leaves) {
    leaf->validateOffsets();if(leaf->pointCount()!=leaf->constAttributeArray("P").size())
      throw std::invalid_argument("point motion P lengths disagree with native point offsets");
    for(size_t a=0;a<leaf->attributeSet().size();++a)leaf->constAttributeArray(a).loadData();
  }
  file.close();
  std::sort(r.leaves.begin(),r.leaves.end(),[](const Leaf* a,const Leaf* b){return std::tuple{a->origin().x(),a->origin().y(),a->origin().z()}<std::tuple{b->origin().x(),b->origin().y(),b->origin().z()};});
  r.schema=describe(*r.grid,r.leaves,pointCount);
  for(const auto* key:ArchiveKeys)r.archived|=bool((*r.grid)[key]);
  if(r.archived) {
    const long double archiveWork=4.0L*r.snapshot->encoded+16*r.metadata;
    if(r.validationWork+archiveWork>o.maxWork)throw std::invalid_argument("point motion archive validation exceeds maxWork before nested snapshot");
    r.validationWork+=archiveWork;
    auto nested=p;budget(r.resident+Fixed+1,p,"point motion archive validation exceeds shared memory budget");
    nested.maxInputBytes-=uintmax_t(std::ceil(r.resident));
    const auto counted=CountSelectedPoints(r.snapshot->file,o.groups,"descriptor:"+chosen,nested,limits);
    if(!counted.archivedSchema || pointCount!=0)throw std::invalid_argument("point motion invalid empty-schema archive");
    r.schema=counted.schema;
  }
  selection(o.groups,r.schema,limits);return r;
#endif
}
struct Record {
  const Leaf* source=nullptr;
  openvdb::Index local=0;
  V destination, world;
  C voxel;
  openvdb::Vec3f encoded;
  bool active=false,selected=false;
};
struct Cell {uint64_t count=0,active=0;};
C aligned(const C& c,int n){return C(c[0]&~(n-1),c[1]&~(n-1),c[2]&~(n-1));}
struct Plan {
  std::vector<Record> records;
  std::map<C,size_t> leafStarts;
  std::map<C,Cell> cells;
  std::set<C> leaves,lower,upper;
  openvdb::math::Transform::Ptr transform;
  long double expanded=0,reserve=0,work=0;
  PointMotionResult report;
};
struct MotionTopologyContext {
  bool joint=false;
  uint64_t externalLeafSlots=0,externalActive=0;
};
Plan plan(const Source& s,const PointMotionOptions& o,const ProcessingOptions& p,
          const std::function<V(const V&)>& trajectory={},double requiredWorldPrecision=0,
          const MotionTopologyContext& topology={}) {
  Plan q;q.transform=o.targetIndexToWorld?targetMap(*o.targetIndexToWorld):detail::CopyTransformExact(s.grid->transform());
  const Affine source(s.grid->transform()),target(*q.transform,bool(o.targetIndexToWorld));
  const bool same=sameMap(s.grid->transform(),*q.transform),id=identity(o.worldFromWorld);
  q.report.inputCount=q.report.outputCount=s.schema.pointCount;q.report.archivedSchema=s.archived;
  q.report.noOp=true;q.work=s.validationWork;q.records.reserve(size_t(s.schema.pointCount));
  for(const auto* leaf:s.leaves) {
    q.leafStarts.emplace(leaf->origin(),q.records.size());Predicate selected(*leaf,o.groups);
    points::AttributeHandle<openvdb::Vec3f> positions(leaf->constAttributeArray("P"));
    for(auto i=leaf->beginIndexAll();i;++i) {
      const auto local=positions.get(*i);Record r;r.source=leaf;r.local=*i;r.active=leaf->isValueOn(i.getCoord());r.selected=selected(*i);
      q.report.selectedCount+=r.selected;q.report.inputActiveCount+=r.active;
      const V original=source.world(i.getCoord(),local,r.selected&&trajectory?requiredWorldPrecision:0);
      r.world=r.selected&&trajectory?trajectory(original):(r.selected&&!id?deform(original,o.worldFromWorld,target.minimum):original);
      r.destination=(same && ((!trajectory && (!r.selected || id)) || (trajectory && r.world==original)))?i.getCoord().asVec3d()+V(local):target.index(r.world);
      for(size_t a=0;a<3;++a)if(std::abs(r.destination[a])>double(INT32_MAX)-8192 || spacing(r.destination[a])>1e-4)
        throw std::invalid_argument("point motion destination exceeds native coordinate/precision bounds");
      r.voxel=C::round(r.destination);
      q.report.noOp&=same && r.voxel==i.getCoord() && r.destination==i.getCoord().asVec3d()+V(local);
      auto& cell=q.cells[r.voxel];++cell.count;cell.active+=r.active;
      if(cell.count>uint64_t(INT32_MAX))throw std::invalid_argument("point motion destination voxel exceeds native Int32 point offsets");
      q.leaves.insert(aligned(r.voxel,8));q.lower.insert(aligned(r.voxel,128));q.upper.insert(aligned(r.voxel,4096));
      q.records.push_back(r);
    }
  }
  if(q.records.size()!=s.schema.pointCount)throw std::runtime_error("point motion source iterator lost records");
  if(q.records.empty())q.report.noOp=true;
  const auto outputLeaves=q.report.noOp?s.leaves.size():q.leaves.size();
  const long double outputMetadata=metadataBytes(*s.grid)+metadataBytes(*s.fileMetadata)+4096+
      (s.leaves.empty()?0:outputLeaves*metadataBytes(s.leaves.front()->attributeSet().descriptor().getMetadata()));
  if(s.metadata+outputMetadata>o.maxMetadataBytes)
    throw std::invalid_argument("point motion aggregate source/output descriptor metadata exceeds maxMetadataBytes");
  q.work+=8*outputMetadata;
  for(const auto& [c,cell]:q.cells) {
    (void)c;if(cell.active && cell.active<cell.count) {
      if(!q.report.noOp && o.rejectActivityMerges)throw std::invalid_argument("point motion merges active/inactive records in one destination voxel; opt into OR activity explicitly");
      q.report.inactiveRecordsActivated+=cell.count-cell.active;
    }
    q.report.outputActiveCount+=cell.active?cell.count:0;
  }
  q.report.outputVoxelCount=q.cells.size();
  if(topology.joint) {
    // Original immutable points, private source clone, and native destination
    // count/output leaves coexist. PointMask steals count leaves into output;
    // writer views share that final tree. Count clones even for exact copies.
    const long double sourceSlots=s.leaves.size()*512.0L;
    const long double targetSlots=q.report.noOp?0:q.leaves.size()*512.0L;
    const long double active=q.report.noOp?2.0L*s.grid->activeVoxelCount():
        s.grid->activeVoxelCount()+sourceSlots+q.cells.size();
    if(2*sourceSlots+targetSlots+topology.externalLeafSlots>p.maxVoxelCount ||
       active+topology.externalActive>p.maxVoxelCount)
      throw std::invalid_argument("point motion joint retained/source-clone/target topology exceeds maxVoxelCount before native movement");
  }
  if(q.report.noOp) {
    q.report.maxPositionErrorVoxels=q.report.maxPositionErrorWorld=q.report.maxUnselectedPositionErrorVoxels=0;
    q.report.outputActiveCount=q.report.inputActiveCount;q.report.inactiveRecordsActivated=0;
    q.work+=q.records.size()*64.0L;
    if(q.work>o.maxWork)throw std::invalid_argument("point motion exact-copy validation exceeds maxWork");
    return q;
  }
  if(q.leaves.size()>p.maxVoxelCount/512 || q.cells.size()>p.maxVoxelCount || q.leaves.size()>UINT32_MAX)
    throw std::invalid_argument("point motion target occupied/leaf topology exceeds maxVoxelCount/native indices");
  const auto* first=s.leaves.empty()?nullptr:s.leaves.front();
  long double perPoint=8,attributeWork=8;
  if(first)for(size_t a=0;a<first->attributeSet().size();++a) {
    const auto& exemplar=first->constAttributeArray(a);
    if(!exemplar.hasConstantStride())throw std::invalid_argument("point motion requires fixed-stride attributes; dynamic layouts are unsupported for repartitioning");
    const long double width=static_cast<long double>(exemplar.stride())*exemplar.storageTypeSize();
    if(!exemplar.stride() || width>UINT32_MAX || s.schema.pointCount*static_cast<long double>(exemplar.stride())>UINT32_MAX)
      throw std::invalid_argument("point motion expanded strided storage exceeds native Index32 range");
    perPoint+=width;attributeWork+=width;
    for(const auto* leaf:s.leaves) {
      const auto& array=leaf->constAttributeArray(a);
      if(!array.hasConstantStride() || array.stride()!=exemplar.stride() || array.isHidden()!=exemplar.isHidden() || array.type()!=exemplar.type())
        throw std::invalid_argument("point motion requires matching persistent attribute flags/types/strides across leaves");
      if(array.isUniform() && array.stride()>1)q.expanded+=leaf->pointCount()*width+512;
    }
  }
  const long double branch=q.leaves.size()*(sizeof(Leaf)+8192.0L+(first?first->attributeSet().size():0)*1024.0L)+
      q.lower.size()*(sizeof(Lower)+8192.0L)+q.upper.size()*(sizeof(Upper)+8192.0L);
  const long double arrays=s.schema.pointCount*perPoint+q.leaves.size()*4096.0L;
  const long double slots=q.leaves.size()*512.0L+q.lower.size()*Lower::NUM_VALUES*1.0L+q.upper.size()*Upper::NUM_VALUES*1.0L;
  q.work+=q.records.size()*(2048.0L+16*attributeWork+64*std::ceil(std::log2(double(q.records.size())+1)))+16*slots;
  q.reserve=s.resident+8*q.expanded+16*branch+16*arrays+q.records.size()*2048.0L+16*(s.metadata+outputMetadata)+Fixed;
  budget(q.reserve,p,"point motion native tree/expanded attributes/move maps/writer exceeds shared memory budget");
  if(q.work>o.maxWork)throw std::invalid_argument("point motion native repartition/payload/tree work exceeds maxWork");
  // An exact copy never re-encodes P. Defer both creation of a writable codec
  // handle and the trial until all records have established movement is needed.
  auto trial=points::AttributeArray::create(first->constAttributeArray("P").type(),1);
  points::AttributeWriteHandle<openvdb::Vec3f> encode(*trial);
  points::AttributeHandle<openvdb::Vec3f> decode(*trial);
  for(auto& r:q.records) {
    const V offset=r.destination-r.voxel.asVec3d();
    encode.set(0,openvdb::Vec3f(offset));r.encoded=decode.get(0);
    double error=0;for(size_t a=0;a<3;++a) {
      if(!std::isfinite(r.encoded[a]) || std::abs(r.encoded[a])>.500001f)
        throw std::invalid_argument("point motion P codec cannot encode finite voxel-local offsets");
      error=std::max(error,std::abs(double(r.encoded[a])-offset[a]));
    }
    const V actual=target.world(r.voxel,r.encoded),delta=actual-r.world;
    for(size_t a=0;a<3;++a)error=std::max(error,std::abs(target.inverseRows[a].dot(delta)));
    if(error>o.maxPositionErrorVoxels)
      throw std::invalid_argument("point motion P codec/index quantization exceeds maxPositionErrorVoxels; choose a suitable explicit tolerance or map");
    q.report.maxPositionErrorVoxels=std::max(q.report.maxPositionErrorVoxels,error);
    q.report.maxPositionErrorWorld=std::max(q.report.maxPositionErrorWorld,std::hypot(delta[0],delta[1],delta[2]));
    if(!r.selected)q.report.maxUnselectedPositionErrorVoxels=std::max(q.report.maxUnselectedPositionErrorVoxels,error);
  }
  // A native leaf's cumulative histogram uses Int32 offsets.
  std::map<C,uint64_t> counts;for(const auto& [c,cell]:q.cells)if((counts[aligned(c,8)]+=cell.count)>uint64_t(INT32_MAX))
    throw std::invalid_argument("point motion target leaf exceeds native Int32 cumulative point offsets");
  return q;
}
struct CachedDestination {
  const Plan* plan=nullptr;size_t start=0;
  template<class L>void reset(const L& leaf,size_t) {start=plan->leafStarts.at(leaf.origin());}
  template<class I>void apply(V& position,const I& i)const {position=plan->records.at(start+size_t(*i)).destination;}
};
bool payloadEqual(const points::AttributeArray& a,openvdb::Index source,const points::AttributeArray& b,openvdb::Index target) {
  const size_t width=a.storageTypeSize(),stride=a.stride();
  const char* x=a.constDataAsByteArray();const char* y=b.constDataAsByteArray();
  for(size_t n=0;n<stride;++n) {
    const size_t xa=a.isUniform()?0:(size_t(source)*stride+n)*width;
    const size_t yb=b.isUniform()?0:(size_t(target)*stride+n)*width;
    if(std::memcmp(x+xa,y+yb,width)!=0)return false;
  }
  return true;
}
Grid::Ptr move(const Source& s,Plan& q,const PointMotionOptions& o,const ProcessingOptions& p) {
  auto result=detail::DeepCopyGridExact(*s.grid);
  if(q.report.noOp) {
    if(q.records.empty())result->setTransform(detail::CopyTransformExact(*q.transform));
    return result;
  }
  std::string reference="container_p05_original_record";
  for(size_t n=0;!s.leaves.empty() && s.leaves.front()->hasAttribute(reference);++n)reference="container_p05_original_record_"+std::to_string(n);
  points::appendAttribute<int64_t>(result->tree(),reference);
  for(auto leaf=result->tree().beginLeaf();leaf;++leaf) {
    const auto start=q.leafStarts.at(leaf->origin());
    points::AttributeWriteHandle<int64_t> handle(leaf->attributeArray(reference));
    for(openvdb::Index n=0;n<leaf->pointCount();++n)handle.set(n,int64_t(start+n));
    for(size_t a=0;a<leaf->attributeSet().size();++a) {
      auto& array=leaf->attributeArray(a);array.loadData();if(array.isUniform() && array.stride()>1)array.expand();
    }
    // PointMove visits beginIndexOn. Occupied inactive records must participate
    // in both native passes; native final activity is restored below.
    leaf->setValuesOn();
  }
  auto unit=openvdb::math::Transform::createLinearTransform(1);
  result->setTransform(unit);CachedDestination deformer{&q};
  points::movePoints(*result,deformer,points::NullFilter(),nullptr,false);
  result->setTransform(detail::CopyTransformExact(*q.transform));
  if(result->tree().leafCount()!=q.leaves.size())throw std::runtime_error("native point move disagrees with planned target leaves");
  std::vector<uint8_t> seen(q.records.size());uint64_t count=0,active=0;
  for(auto leaf=result->tree().beginLeaf();leaf;++leaf) {
    leaf->validateOffsets();leaf->setValuesOff();
    points::AttributeHandle<int64_t> refs(leaf->constAttributeArray(reference));
    points::AttributeHandle<openvdb::Vec3f> positions(leaf->constAttributeArray("P"));
    for(auto i=leaf->beginIndexAll();i;++i) {
      const auto ref=refs.get(*i);
      if(ref<0 || uint64_t(ref)>=q.records.size() || seen[size_t(ref)]++)throw std::runtime_error("native point move lost/duplicated original record references");
      const auto& original=q.records[size_t(ref)];
      if(i.getCoord()!=original.voxel || positions.get(*i)!=original.encoded)
        throw std::runtime_error("native point move position/codec differs from preflight");
      for(size_t a=0;a<original.source->attributeSet().size();++a) {
        const auto& before=original.source->constAttributeArray(a);
        const auto& after=leaf->constAttributeArray(a);
        if(before.type()!=after.type() || before.stride()!=after.stride() || before.isHidden()!=after.isHidden())
          throw std::runtime_error("native point move changed persistent attribute type/stride/flags");
        if(a!=original.source->attributeSet().find("P") && !payloadEqual(before,original.local,after,*i))
          throw std::runtime_error("native point move changed non-P attribute storage bits");
      }
      if(q.cells.at(i.getCoord()).active){leaf->setActiveState(i.getCoord(),true);++active;}
      ++count;
    }
  }
  if(count!=s.schema.pointCount || active!=q.report.outputActiveCount || std::find(seen.begin(),seen.end(),0)!=seen.end())
    throw std::runtime_error("native point move did not preserve all original records/activity");
  points::dropAttribute(result->tree(),reference);
  for(auto leaf=result->tree().cbeginLeaf();leaf;++leaf)if(leaf->attributeSet().descriptor()!=s.leaves.front()->attributeSet().descriptor())
    throw std::runtime_error("native point move changed descriptor metadata/groups");
  (void)o;(void)p;return result;
}
class ExactWriteGrid:public Grid {
public:
  explicit ExactWriteGrid(const Grid& grid):Grid(openvdb::ConstPtrCast<Tree>(grid.constTreePtr())) {
    insertMeta(grid);setTransform(detail::CopyTransformExact(grid.transform()));
  }
  openvdb::GridBase::ConstPtr copyGrid()const override{return copyReplacingMetadataAndTransform(*this,detail::CopyTransformExact(transform()));}
  openvdb::GridBase::Ptr copyGrid()override{return openvdb::ConstPtrCast<openvdb::GridBase>(static_cast<const ExactWriteGrid&>(*this).copyGrid());}
};
void publish(const std::filesystem::path& source,const std::filesystem::path& output) {
  bool owned=false;std::ofstream target;
  try {
    std::ifstream input(source,std::ios::binary);if(!input)throw std::runtime_error("point motion cannot read completed output");
    target.open(output,std::ios::binary|std::ios::out|std::ios::noreplace);owned=target.is_open();
    if(!owned)throw std::runtime_error("point motion cannot exclusively create output");
    target.exceptions(std::ios::badbit|std::ios::failbit);input.exceptions(std::ios::badbit);
    std::array<char,65536> buffer{};
    while(input.read(buffer.data(),std::streamsize(buffer.size())) || input.gcount())target.write(buffer.data(),input.gcount());
    if(!input.eof())throw std::runtime_error("point motion completed output read failed");target.close();
  }catch(...) {
    target.exceptions(std::ios::goodbit);if(target.is_open())target.close();
    if(owned){std::error_code e;std::filesystem::remove(output,e);}throw;
  }
}
PointMotionResult finishMovement(Source& source,Plan& q,const std::filesystem::path& output,
    const PointMotionOptions& o,const ProcessingOptions& p,
    const std::function<void(PointMotionResult&)>& beforePublish={}) {
  auto target=move(source,q,o,p);
  target->setName(p.gridName);target->removeMeta("container:point_count");target->insertMeta("container:point_count",openvdb::Int64Metadata(int64_t(q.report.outputCount)));
  target->setSaveFloatAsHalf(false);
  std::vector<const Leaf*> leaves;for(auto leaf=target->tree().cbeginLeaf();leaf;++leaf)leaves.push_back(&*leaf);
  q.report.schema=source.archived?source.schema:describe(*target,leaves,q.report.outputCount);
  q.report.schema.gridName=p.gridName;q.report.schema.leafCount=leaves.size();
  q.report.workUpperBound=uint64_t(std::ceil(q.work));
  q.report.diagnostics.emplace_back("Native PointMove preserves non-P payload storage; only P is deformed, and native record order may change.");
  q.report.diagnostics.emplace_back(q.report.noOp?"Exact no-op retains native tree, codecs and point activity.":"All records, including inactive records, were repartitioned and checked against original references.");
  if(q.report.inactiveRecordsActivated)q.report.diagnostics.emplace_back("Explicit OR activity merged active/inactive records; inactiveRecordsActivated reports affected records.");
  const long double finalBytes=openvdb::tools::memUsageIfLoaded(target->tree(),false)+8*metadataBytes(*target);
  budget(source.resident+q.reserve+8*finalBytes+Fixed,p,"point motion final native output/writer exceeds shared memory budget");
  const auto temporary=source.snapshot->directory/"moved.vdb";
  struct Cleanup {std::filesystem::path file;~Cleanup(){std::error_code e;std::filesystem::remove(file,e);}}cleanup{temporary};
  openvdb::GridPtrVec grids{std::make_shared<ExactWriteGrid>(*target)};
  openvdb::io::File writer(temporary.string());writer.setCompression(openvdb::io::COMPRESS_ZIP);writer.write(grids,*source.fileMetadata);writer.close();
  budget(source.resident+q.reserve+8*finalBytes+std::filesystem::file_size(temporary)+Fixed,p,"point motion encoded output exceeds shared memory budget");
  static_assert(std::is_nothrow_move_constructible_v<PointMotionResult>);
  if(beforePublish)beforePublish(q.report);
  publish(temporary,output);return std::move(q.report);
}
PointMotionResult execute(const std::filesystem::path& input,const std::filesystem::path& output,const PointMotionOptions& o,const std::string& selector,const ProcessingOptions& p,const PointAttributeOptions& limits) {
  auto source=loadSource(input,selector,o,p,limits);auto q=plan(source,o,p);
  return finishMovement(source,q,output,o,p);
}
#endif
} // namespace
} // namespace container::geometry::vdb
