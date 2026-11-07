#include "Container/geometry/OpenVdbPointTransfer.h"
#include "OpenVdbProvenance.h"
#include <stdexcept>
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
#include "OpenVdbPointAttributesInternal.h"
#include <openvdb/io/GridDescriptor.h>
#include <openvdb/points/PointTransfer.h>
#include <openvdb/points/PointConversion.h>
#include <openvdb/tools/PointIndexGrid.h>
#include <openvdb/tools/Count.h>
#include <tbb/task_arena.h>
#include <bit>
#include <map>
#include <sstream>
#include <type_traits>
#include <variant>
#endif
namespace container::geometry::vdb {
namespace {
[[noreturn]] void unavailable() {
  throw std::runtime_error("OpenVDB generic point transfer is unavailable; configure with -DENABLE_OPENVDB=ON.");
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
      throw std::invalid_argument("point transfer coordinate product overflows or underflows");
    add(x); add(std::fma(a,b,-x));
  }
  double get() const {
    const double x=value+correction;
    if(!std::isfinite(x)) throw std::invalid_argument("point transfer coordinate sum is nonfinite");
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
    if(!t.isLinear()) throw std::invalid_argument("point transfer requires affine maps");
    origin=t.indexToWorld(V(0));
    const auto m=t.baseMap();
    axes={m->applyJacobian({1,0,0}),m->applyJacobian({0,1,0}),m->applyJacobian({0,0,1})};
    auto unit=axes;
    double lengths[3];
    for(size_t a=0;a<3;++a) {
      lengths[a]=std::hypot(axes[a][0],axes[a][1],axes[a][2]);
      if(!std::isfinite(origin[a]) || !std::isfinite(lengths[a]) || lengths[a]<1e-6 || lengths[a]>1e6)
        throw std::invalid_argument("point transfer affine axes must span [1e-6,1e6] world units");
      minimum=std::min(minimum,lengths[a]); maximum=std::max(maximum,lengths[a]); unit[a]/=lengths[a];
    }
    const double det=unit[0].dot(unit[1].cross(unit[2]));
    if(!std::isfinite(det) || std::abs(det)<1e-8)
      throw std::invalid_argument("point transfer affine is singular or insufficiently conditioned");
    if(uniform) for(size_t a=0;a<3;++a) {
      if(std::abs(lengths[a]-lengths[0])>lengths[0]*1e-12)
        throw std::invalid_argument("point transfer target axes must have uniform lengths");
      for(size_t b=a+1;b<3;++b) if(std::abs(unit[a].dot(unit[b]))>1e-12)
        throw std::invalid_argument("point transfer target axes must be orthogonal");
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
  V world(const C& c,const openvdb::Vec3f& local) const {
    V result;
    for(size_t a=0;a<3;++a) if(!std::isfinite(local[a]) || std::abs(local[a])>.500001f)
      throw std::invalid_argument("point transfer P must have finite local offsets in [-.5,.5]");
    for(size_t row=0;row<3;++row) {
      Sum s;s.add(origin[row]);
      for(size_t a=0;a<3;++a) {s.product(axes[a][row],double(c[a]));s.product(axes[a][row],double(local[a]));}
      result[row]=s.get();
      if(s.uncertainty()>minimum*1e-4)
        throw std::invalid_argument("point transfer world position loses source voxel precision; recenter the source");
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
      throw std::invalid_argument("point transfer target inverse/world arithmetic loses voxel precision; recenter or condition maps");
    V result;
    for(size_t a=0;a<3;++a) {
      Sum s;for(size_t b=0;b<3;++b)s.product(inverseRows[a][b],delta[b]);result[a]=s.get();
      if(std::abs(result[a])>double(INT32_MAX)-8192 || spacing(result[a])>1e-4)
        throw std::invalid_argument("point transfer destination exceeds native coordinate/precision bounds");
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
  for(double x:m)if(!std::isfinite(x))throw std::invalid_argument("point transfer matrix entries must be finite");
  if(m[12]!=0 || m[13]!=0 || m[14]!=0 || m[15]!=1)
    throw std::invalid_argument("point transfer matrix last row must equal 0,0,0,1");
}
openvdb::math::Transform::Ptr targetMap(const std::array<double,16>& m) {
  matrix(m);
  if(m[0]!=0 && m[5]!=0 && m[10]!=0 &&
     m[1]==0 && m[2]==0 && m[4]==0 && m[6]==0 && m[8]==0 && m[9]==0) {
    const V scales(m[0],m[5],m[10]),voxels(std::abs(m[0]),std::abs(m[5]),std::abs(m[10]));
    for(size_t a=0;a<3;++a)if(voxels[a]<1e-6 || voxels[a]>1e6)throw std::invalid_argument("point transfer target scales must be in [1e-6,1e6]");
    std::stringstream record(std::ios::in|std::ios::out|std::ios::binary);
    record.exceptions(std::ios::badbit|std::ios::failbit);
    const bool positiveUniform=scales[0]>0 && scales[0]==scales[1] && scales[0]==scales[2];
    openvdb::writeString(record,positiveUniform?openvdb::math::UniformScaleTranslateMap::mapType():openvdb::math::ScaleTranslateMap::mapType());
    V(m[3],m[7],m[11]).write(record);
    scales.write(record);voxels.write(record);
    V(1/scales[0],1/scales[1],1/scales[2]).write(record);
    V(1/(scales[0]*scales[0]),1/(scales[1]*scales[1]),1/(scales[2]*scales[2])).write(record);
    V(.5/scales[0],.5/scales[1],.5/scales[2]).write(record);
    record.seekg(0);openvdb::io::setCurrentVersion(record);
    auto result=std::make_shared<openvdb::math::Transform>();result->read(record);return result;
  }
  openvdb::math::Mat4d native;
  for(size_t a=0;a<4;++a)for(size_t b=0;b<4;++b)native[a][b]=m[4*b+a];
  try {return detail::MakeAffineTransformExact(native);}
  catch(const openvdb::ArithmeticError&) {
    throw std::invalid_argument("point transfer target map cannot be represented by native AffineMap; tiny rotated maps exceed SDK determinant limits");
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
    if(++terms>3*limits.maxGroups)throw std::invalid_argument("point transfer group predicate exceeds count cap");
    name(n);if(std::find(schema.groups.begin(),schema.groups.end(),n)==schema.groups.end())
      throw std::invalid_argument("point transfer selected group was not found: "+n);
  }
}
class MetadataArchive:public openvdb::io::Archive {
public:
  void begin(std::istream& s,openvdb::io::StreamMetadata::Ptr& m) {
    readHeader(s);
    if(!inputHasGridOffsets() || fileVersion()<openvdb::OPENVDB_FILE_VERSION_GRID_INSTANCING || fileVersion()>openvdb::OPENVDB_FILE_VERSION)
      throw std::invalid_argument("point transfer requires supported seekable metadata-readable VDB");
    openvdb::io::setStreamMetadataPtr(s,m,false);setFormatVersion(s);setLibraryVersion(s);setDataCompression(s);
  }
  int32_t count(std::istream& s){return readGridCount(s);}
  openvdb::GridBase::Ptr next(std::istream& s,openvdb::io::GridDescriptor& d,uintmax_t bytes) {
    auto grid=d.read(s);const auto after=int64_t(s.tellg()),start=d.getGridPos(),end=d.getEndPos(),blocks=d.getBlockPos();
    if(!grid || start<after || end<=start || uintmax_t(end)>bytes || (!d.isInstance() && (blocks<start || blocks>end)))
      throw std::invalid_argument("point transfer descriptor offsets exceed encoded file");
    d.seekToGrid(s);readGridCompression(s);grid->readMeta(s);grid->readTransform(s);
    if(fileVersion()<openvdb::OPENVDB_FILE_VERSION_NO_GRIDMAP && grid->getName().empty())grid->setName(d.gridName());
    if(int64_t(s.tellg())>(d.isInstance()?end:blocks))throw std::invalid_argument("point transfer metadata crosses payload boundary");
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
};
Source loadSource(const std::filesystem::path& input,const std::string& selector,const PointTransferOptions& o,const ProcessingOptions& p,const PointAttributeOptions& limits) {
  Source r;r.snapshot=std::make_unique<Snapshot>(input,p);detail::RequireSeekableFile(r.snapshot->file);
  if(!openvdb::io::Archive::isDelayedLoadingEnabled())
    throw std::invalid_argument("bounded point transfer requires delayed loading; unset OPENVDB_DISABLE_DELAYED_LOAD");
#ifndef OPENVDB_USE_DELAYED_LOADING
  throw std::invalid_argument("bounded point transfer requires delayed-loading support");
#else
  auto sm=std::make_shared<openvdb::io::StreamMetadata>();sm->setSeekable(true);
  std::ifstream stream(r.snapshot->file,std::ios::binary);stream.exceptions(std::ios::badbit|std::ios::failbit);
  MetadataArchive archive;archive.begin(stream,sm);
  r.fileMetadata=std::make_shared<openvdb::MetaMap>();r.fileMetadata->readMeta(stream);
  r.metadata=metadataBytes(*r.fileMetadata);long double catalog=r.snapshot->encoded+Fixed;
  const auto check=[&] {
    if(r.metadata>o.maxMetadataBytes)throw std::invalid_argument("point transfer aggregate file/grid/descriptor metadata exceeds maxMetadataBytes");
    budget(catalog+8*r.metadata,p,"point transfer catalog/metadata exceeds memory budget");
  };check();
  const int32_t count=archive.count(stream);
  if(count<0 || uint32_t(count)>o.maxGrids)throw std::invalid_argument("point transfer catalog exceeds maxGrids");
  std::set<std::string> descriptors;std::string chosen;openvdb::math::Transform::Ptr exact;
  bool found=false;
  for(int32_t i=0;i<count;++i) {
    openvdb::io::GridDescriptor d;auto grid=archive.next(stream,d,r.snapshot->encoded);
    const auto n=openvdb::io::GridDescriptor::nameAsString(d.uniqueName());catalog+=4.0L*n.size()+256;
    if(!descriptors.insert(n).second)throw std::invalid_argument("point transfer input has colliding grid descriptors");
    r.metadata+=metadataBytes(*grid);check();
    if(!grid->isType<Grid>() || !GridSelectionMatches(selector,n,grid->getName()))continue;
    if(found)throw std::invalid_argument("point transfer grid is ambiguous; use descriptor:<exact name>");
    const auto memory=grid->getMetadata<openvdb::Int64Metadata>(openvdb::GridBase::META_FILE_MEM_BYTES),
               voxels=grid->getMetadata<openvdb::Int64Metadata>(openvdb::GridBase::META_FILE_VOXEL_COUNT);
    if(!memory || !voxels || memory->value()<0 || voxels->value()<0 || uint64_t(voxels->value())>p.maxVoxelCount)
      throw std::invalid_argument("point transfer requires valid recorded memory/voxel statistics within budget");
    budget(catalog+8*r.metadata+memory->value(),p,"recorded point transfer source exceeds memory budget");
    chosen=n;found=true;exact=detail::CopyTransformExact(grid->transform());
  }
  if(!found)throw std::invalid_argument("point transfer found no matching native PointDataGrid");
  stream.close();openvdb::io::File file(r.snapshot->file.string());file.setCopyMaxBytes(0);file.open(true);
  if(file.getUniqueTag()!=archive.getUniqueTag())throw std::runtime_error("point transfer private snapshot changed");
  r.grid=openvdb::gridPtrCast<Grid>(file.readGrid(chosen));
  if(!sameMap(r.grid->transform(),*exact))throw std::runtime_error("point transfer native read changed exact source map");
  r.grid->setTransform(exact);
  const auto& tree=r.grid->tree();
  if(tree.hasActiveTiles() || tree.leafCount()>p.maxVoxelCount/512 || tree.leafCount()>UINT32_MAX || tree.activeVoxelCount()>p.maxVoxelCount)
    throw std::invalid_argument("point transfer stored source topology exceeds voxel/leaf cap");
  r.full=openvdb::tools::memUsageIfLoaded(tree,false);
  const auto nodes=tree.nodeCount();
  r.validationWork=tree.root().getTableSize()+nodes[0]*512.0L+nodes[1]*Lower::NUM_VALUES*1.0L+nodes[2]*Upper::NUM_VALUES*1.0L;
  if(r.validationWork>o.maxWork)throw std::invalid_argument("point transfer source topology validation exceeds maxWork");
  const points::AttributeSet::Descriptor* first=nullptr;uint64_t pointCount=0;uintmax_t dictionary=0;
  for(auto leaf=tree.cbeginLeaf();leaf;++leaf) {
    const auto& descriptor=leaf->attributeSet().descriptor();
    if(!first)first=&descriptor;
    if(!leaf->hasAttribute("P") || descriptor.map().size()>limits.maxAttributes || descriptor.groupMap().size()>limits.maxGroups)
      throw std::invalid_argument("point transfer schema exceeds attribute/group cap or lacks P");
    const auto& pos=leaf->constAttributeArray("P");
    if(!pos.hasValueType<openvdb::Vec3f>() || !pos.hasConstantStride() || pos.stride()!=1)
      throw std::invalid_argument("point transfer P requires registered fixed stride-one Vec3f storage");
    if(pos.size()>limits.maxPoints-std::min<uint64_t>(pointCount,limits.maxPoints))
      throw std::invalid_argument("point transfer source exceeds maxPoints");
    pointCount+=pos.size();
    if(pointCount>UINT32_MAX)throw std::invalid_argument("point transfer native proxy requires at most UInt32 points");
    for(size_t a=0;a<leaf->attributeSet().size();++a) {
      const auto& array=leaf->constAttributeArray(a);
      if(array.size()!=pos.size())throw std::invalid_argument("point transfer attribute lengths disagree");
    }
    for(const auto& [n,offset]:descriptor.groupMap()) {
      (void)offset;const auto index=leaf->attributeSet().groupIndex(n);
      if(index.first>=leaf->attributeSet().size())throw std::invalid_argument("point transfer group backing is invalid");
      const auto& a=leaf->constAttributeArray(index.first);
      if(!points::isGroup(a) || !a.hasConstantStride() || a.stride()!=1)
        throw std::invalid_argument("point transfer groups require fixed stride-one backing arrays");
    }
    for(auto i=descriptor.getMetadata().beginMeta();i!=descriptor.getMetadata().endMeta();++i)if(i->first.starts_with("string:")) {
      const auto text=std::dynamic_pointer_cast<openvdb::StringMetadata>(i->second);
      if(!text || text->value().size()>limits.maxStringBytes-dictionary)
        throw std::invalid_argument("point transfer aggregate native string dictionary exceeds maxStringBytes");
      dictionary+=text->value().size();
    }
    r.metadata+=metadataBytes(descriptor.getMetadata());r.leaves.push_back(&*leaf);check();
  }
  const auto terms=o.groups.anyOf.size()+o.groups.allOf.size()+o.groups.noneOf.size();
  r.validationWork+=pointCount*(4096.0L+4*terms)+r.leaves.size()*limits.maxAttributes*8.0L;
  if(r.validationWork>o.maxWork)throw std::invalid_argument("point transfer position/group validation exceeds maxWork");
  r.resident=catalog+12*r.metadata+4*r.full+pointCount*2048.0L+Fixed;
  budget(r.resident,p,"point transfer full decoded source/planning workspace exceeds memory budget before materialization");
  r.grid->readNonresidentBuffers();
  for(const auto* leaf:r.leaves) {
    leaf->validateOffsets();if(leaf->pointCount()!=leaf->constAttributeArray("P").size())
      throw std::invalid_argument("point transfer P lengths disagree with native point offsets");
  }
  file.close();
  std::sort(r.leaves.begin(),r.leaves.end(),[](const Leaf* a,const Leaf* b){return std::tuple{a->origin().x(),a->origin().y(),a->origin().z()}<std::tuple{b->origin().x(),b->origin().y(),b->origin().z()};});
  r.schema={r.grid->getName(),pointCount,r.leaves.size()};
  if(first)for(const auto& [n,index]:first->groupMap()){(void)index;r.schema.groups.push_back(n);}
  for(const auto* key:ArchiveKeys)r.archived|=bool((*r.grid)[key]);
  if(r.archived) {
    auto nested=p;budget(r.resident+Fixed+1,p,"point transfer archive validation exceeds shared memory budget");
    nested.maxInputBytes-=uintmax_t(std::ceil(r.resident));
    const auto counted=CountSelectedPoints(r.snapshot->file,o.groups,"descriptor:"+chosen,nested,limits);
    if(!counted.archivedSchema || pointCount!=0)throw std::invalid_argument("point transfer invalid empty-schema archive");
    r.schema=counted.schema;
  }
  selection(o.groups,r.schema,limits);return r;
#endif
}

// A finite Double is sig*2^unit, with <=53 significant bits and unit>=-1074.
// Five factors (value,mass,three fractions) have unit>=-5370 and magnitude
// <2^2048. UInt32 point counts add at most32 bits: the canonical numerator
// therefore needs <7450 bits, weights <5352, determinant <3285. The 8192-bit
// capacity leaves >700 bits for exact rational rounding intermediates. Every
// operation also checks its bound; no modular arithmetic can silently wrap.
struct Big {
  static constexpr size_t Limbs=256;
  std::array<uint32_t,Limbs> data{};
  Big(uint64_t n=0){data[0]=uint32_t(n);data[1]=uint32_t(n>>32);}
  size_t bits()const {for(size_t i=Limbs;i--;)if(data[i])return 32*i+32-std::countl_zero(data[i]);return 0;}
  bool zero()const{return bits()==0;}
  int compare(const Big& b)const {for(size_t i=Limbs;i--;)if(data[i]!=b.data[i])return data[i]<b.data[i]?-1:1;return 0;}
  void add(const Big& b) {
    uint64_t carry=0;for(size_t i=0;i<Limbs;++i){const auto sum=uint64_t(data[i])+b.data[i]+carry;data[i]=uint32_t(sum);carry=sum>>32;}
    if(carry)throw std::runtime_error("point transfer exact accumulator capacity exceeded");
  }
  void subtract(const Big& b) {
    if(compare(b)<0)throw std::runtime_error("point transfer negative unsigned intermediate");
    uint64_t borrow=0;for(size_t i=0;i<Limbs;++i){const auto sub=uint64_t(b.data[i])+borrow,old=uint64_t(data[i]);data[i]=uint32_t(old-sub);borrow=old<sub;}
  }
  void shift(size_t n) {
    if(!n || zero())return;if(bits()+n>Limbs*32)throw std::runtime_error("point transfer exact shift capacity exceeded");
    const auto whole=n/32,part=n%32;
    for(size_t i=Limbs;i--;) {uint64_t value=0;if(i>=whole){value=uint64_t(data[i-whole])<<part;if(part && i>whole)value|=uint64_t(data[i-whole-1])>>(32-part);}data[i]=uint32_t(value);}
  }
  size_t trailing()const {for(size_t i=0;i<Limbs;++i)if(data[i])return 32*i+std::countr_zero(data[i]);return 0;}
  void right(size_t n) {
    const auto whole=n/32,part=n%32;
    for(size_t i=0;i<Limbs;++i){uint64_t value=0;if(i+whole<Limbs){value=uint64_t(data[i+whole])>>part;if(part && i+whole+1<Limbs)value|=uint64_t(data[i+whole+1])<<(32-part);}data[i]=uint32_t(value);}
  }
  static Big multiply(const Big& a,const Big& b) {
    Big result;const size_t an=(a.bits()+31)/32,bn=(b.bits()+31)/32;
    if(a.zero() || b.zero())return result;
    if(a.bits()+b.bits()>Limbs*32+1)throw std::runtime_error("point transfer exact product capacity exceeded");
    for(size_t i=0;i<an;++i) {
      uint64_t carry=0;for(size_t j=0;j<bn;++j) {
        if(i+j>=Limbs)throw std::runtime_error("point transfer exact product capacity exceeded");
        const auto product=uint64_t(a.data[i])*b.data[j]+result.data[i+j]+carry;
        result.data[i+j]=uint32_t(product);carry=product>>32;
      }
      if(carry){if(i+bn>=Limbs)throw std::runtime_error("point transfer exact product capacity exceeded");result.data[i+bn]=uint32_t(carry);}
    }
    return result;
  }
};
struct SignedBig {
  Big magnitude;bool negative=false;
  void add(const Big& b,bool sign=false) {
    if(sign==negative)magnitude.add(b);
    else if(magnitude.compare(b)>=0)magnitude.subtract(b);
    else{auto next=b;next.subtract(magnitude);magnitude=next;negative=sign;}
    if(magnitude.zero())negative=false;
  }
};
struct Dyad {Big magnitude;int unit=0;bool negative=false;};
Dyad binary(double value) {
  if(!std::isfinite(value))throw std::invalid_argument("point transfer selected numeric values must be finite");
  const auto bits=std::bit_cast<uint64_t>(value),exponent=(bits>>52)&2047;
  uint64_t sig=(bits&UINT64_C(0xfffffffffffff))|(exponent?UINT64_C(0x10000000000000):0);
  if(!sig)return {};
  const auto trailing=std::countr_zero(sig);sig>>=trailing;
  return {Big(sig),(exponent?int(exponent)-1075:-1074)+int(trailing),bool(bits>>63)};
}
Dyad multiply(const Dyad& a,const Dyad& b) {
  return {Big::multiply(a.magnitude,b.magnitude),a.unit+b.unit,a.negative!=b.negative};
}
Big units(const Dyad& value,int unit) {
  if(value.unit<unit)throw std::runtime_error("point transfer canonical exponent proof failed");
  auto result=value.magnitude;result.shift(size_t(value.unit-unit));return result;
}
std::array<Dyad,2> axisFactors(double coordinate,int32_t base) {
  // q-floor(q) is NOT generally a Double: negative tiny q has a fraction
  // 1-tiny, which rounds to1 if subtraction happens before exact conversion.
  const auto q=binary(coordinate),b=binary(double(base));
  const int unit=std::min(q.unit,0);
  SignedBig fraction;fraction.add(units(q,unit),q.negative);fraction.add(units(b,unit),!b.negative);
  Big one(1);one.shift(size_t(-unit));
  if(fraction.negative || fraction.magnitude.compare(one)>=0)
    throw std::runtime_error("point transfer invalid exact q-floor(q) fraction");
  auto left=one;left.subtract(fraction.magnitude);
  const auto normalized=[](Big magnitude,int exponent) {
    if(magnitude.zero())return Dyad{};
    const auto trailing=magnitude.trailing();magnitude.right(trailing);
    return Dyad{magnitude,exponent+int(trailing),false};
  };
  return {normalized(left,unit),normalized(fraction.magnitude,unit)};
}
double ratio(const Big& numerator,const Big& denominator,int unit,bool residual=false) {
  if(numerator.zero())return 0;if(denominator.zero())throw std::runtime_error("point transfer zero exact denominator");
  int exponent=int(numerator.bits())-int(denominator.bits());
  if(exponent>=0){auto comparison=denominator;comparison.shift(size_t(exponent));if(numerator.compare(comparison)<0)--exponent;}
  else{auto comparison=numerator;comparison.shift(size_t(-exponent));if(comparison.compare(denominator)<0)--exponent;}
  const int target=std::max(exponent+unit-52,-1074),shift=unit-target;
  auto dividend=numerator,divisor=denominator;
  if(shift>=0)dividend.shift(size_t(shift));else divisor.shift(size_t(-shift));
  uint64_t quotient=0;
  for(int bit=53;bit>=0;--bit) {
    auto candidate=divisor;candidate.shift(size_t(bit));
    if(dividend.compare(candidate)>=0){dividend.subtract(candidate);quotient|=uint64_t(1)<<bit;}
  }
  dividend.shift(1);const int order=dividend.compare(divisor);
  if(order>0 || (order==0 && (quotient&1)))++quotient;
  if(target>971 || (target==971 && quotient>=UINT64_C(0x20000000000000)))
    throw std::invalid_argument("point transfer native output overflows Double");
  const double result=std::scalbn(double(quotient),target);
  if(!std::isfinite(result))throw std::invalid_argument("point transfer native output overflows Double");
  if(result==0) {
    if(residual)return std::numeric_limits<double>::denorm_min();
    throw std::invalid_argument("point transfer nonzero native output underflows Double");
  }
  if(residual) {
    const double upper=std::nextafter(result,INFINITY);
    if(!std::isfinite(upper))throw std::invalid_argument("point transfer mass-error bound exceeds Double range");
    return upper;
  }
  return result;
}
double signedRatio(const SignedBig& n,const Big& denominator,int unit) {
  const double result=ratio(n.magnitude,denominator,unit);return n.negative?-result:result;
}
double exactVolume(const Affine& map) {
  SignedBig determinant;
  for(const auto& term:std::array<std::array<int,4>,6>{{{0,1,2,0},{1,2,0,0},{2,0,1,0},{2,1,0,1},{1,0,2,1},{0,2,1,1}}}) {
    auto product=multiply(multiply(binary(map.axes[0][term[0]]),binary(map.axes[1][term[1]])),binary(map.axes[2][term[2]]));
    determinant.add(units(product,-3222),product.negative!=bool(term[3]));
  }
  return ratio(determinant.magnitude,Big(1),-3222);
}
double integer(int64_t value,PointTransferIntegers policy,uint64_t& rounded) {
  const uint64_t magnitude=value<0?uint64_t(-(value+1))+1:uint64_t(value);
  const auto width=std::bit_width(magnitude);
  const bool exact=width<=53 || (magnitude&((UINT64_C(1)<<(width-53))-1))==0;
  if(!exact) {
    if(policy==PointTransferIntegers::RejectInexact)throw std::invalid_argument("point transfer Int64 conversion loses integer bits; opt into AllowRounded explicitly");
    ++rounded;
  }
  return double(value);
}
struct Numeric {
  using Handles=std::variant<
    std::unique_ptr<points::AttributeHandle<bool>>,
    std::unique_ptr<points::AttributeHandle<int32_t>>,
    std::unique_ptr<points::AttributeHandle<int64_t>>,
    std::unique_ptr<points::AttributeHandle<float>>,
    std::unique_ptr<points::AttributeHandle<double>>,
    std::unique_ptr<points::AttributeHandle<openvdb::Vec3i>>,
    std::unique_ptr<points::AttributeHandle<openvdb::Vec3f>>,
    std::unique_ptr<points::AttributeHandle<openvdb::Vec3d>>>;
  Handles handle;uint32_t components=1;
  explicit Numeric(const points::AttributeArray& array) {
    if(points::isGroup(array) || !array.hasConstantStride() || array.stride()!=1)
      throw std::invalid_argument("point transfer chosen attributes require nongroup fixed stride1 numeric storage");
#define TYPE(T) if(array.hasValueType<T>()){handle=std::make_unique<points::AttributeHandle<T>>(array);components=openvdb::VecTraits<T>::IsVec?3:1;return;}
    TYPE(bool) TYPE(int32_t) TYPE(int64_t) TYPE(float) TYPE(double)
    TYPE(openvdb::Vec3i) TYPE(openvdb::Vec3f) TYPE(openvdb::Vec3d)
#undef TYPE
    throw std::invalid_argument("point transfer requires Bool/Int32/Int64/Float/Double/Vec3I/S/D numeric attributes");
  }
  std::array<double,3> get(openvdb::Index n,PointTransferIntegers policy,uint64_t& rounded)const {
    return std::visit([&](const auto& h) {
      const auto value=h->get(n);std::array<double,3> result{};
      if constexpr(openvdb::VecTraits<std::decay_t<decltype(value)>>::IsVec) {
        for(size_t a=0;a<3;++a)result[a]=double(value[a]);
      }else if constexpr(std::is_same_v<std::decay_t<decltype(value)>,int64_t>)result[0]=integer(value,policy,rounded);
      else result[0]=double(value);
      for(double x:result)if(!std::isfinite(x))throw std::invalid_argument("point transfer selected numeric values must be finite");
      return result;
    },handle);
  }
};
struct Record {
  C base;
  std::array<double,3> coordinate{},value{};
  double mass=1;
};
struct Nodes {
  std::set<C> cells,leaves,lower,upper;
  void add(const C& c) {
    cells.insert(c);leaves.insert(c&~7);lower.insert(c&~127);upper.insert(c&~4095);
  }
  long double slots()const{return leaves.size()*512.0L+lower.size()*Lower::NUM_VALUES*1.0L+upper.size()*Upper::NUM_VALUES*1.0L;}
};
template<class T>long double treeBytes(const Nodes& n) {
  using L=typename T::LeafNodeType;using U=typename T::RootNodeType::ChildNodeType;using D=typename U::ChildNodeType;
  return n.leaves.size()*(sizeof(L)+512.0L*sizeof(typename T::ValueType)+4096)+
    n.lower.size()*(sizeof(D)+4096.0L)+n.upper.size()*(sizeof(U)+4096.0L);
}
struct LeafResult {
  Big canonicalWeight,roundedIntegral;
  uint64_t contributions=0,roundedFloat=0;
  std::array<double,3> minimum{},maximum{};
  double floatError=0;
  bool hasValues=false;
};
struct Accumulator {SignedBig numerator[3];Big weight;};
struct LeafScratch {std::array<Accumulator,512> values;};
struct Plan {
  std::vector<Record> records;
  Nodes field,proxy;
  openvdb::math::Transform::Ptr transform;
  Big totalMass;
  long double reserve=0,work=0;
  PointTransferResult result;
};
uint32_t archivedComponents(const Source& s,const std::string& attribute) {
  const auto found=std::find_if(s.schema.attributes.begin(),s.schema.attributes.end(),[&](const auto& a){return a.name==attribute;});
  if(found==s.schema.attributes.end() || found->groupStorage || found->stride!=1)
    throw std::invalid_argument("point transfer chosen archived attribute is missing, group or not fixed stride1");
#define TYPE(T,N) if(found->valueType==points::TypedAttributeArray<T>::attributeType().first)return N;
  TYPE(bool,1) TYPE(int32_t,1) TYPE(int64_t,1) TYPE(float,1) TYPE(double,1)
  TYPE(openvdb::Vec3i,3) TYPE(openvdb::Vec3f,3) TYPE(openvdb::Vec3d,3)
#undef TYPE
  throw std::invalid_argument("point transfer archived attribute must have a supported numeric type");
}
Plan plan(const Source& s,const std::string& attribute,const PointTransferOptions& o,const ProcessingOptions& p) {
  Plan q;q.transform=targetMap(o.targetIndexToWorld);const Affine source(s.grid->transform()),target(*q.transform,true);
  q.result.voxelVolume=exactVolume(target);q.result.inputCount=s.schema.pointCount;q.result.archivedSchema=s.archived;
  q.work=s.validationWork;q.records.reserve(size_t(s.schema.pointCount));
  bool first=true;
  for(const auto* leaf:s.leaves) {
    if(!leaf->hasAttribute(attribute))throw std::invalid_argument("point transfer chosen attribute was not found: "+attribute);
    Numeric field(leaf->constAttributeArray(attribute));
    if(first){q.result.componentCount=field.components;first=false;}
    else if(field.components!=q.result.componentCount)throw std::invalid_argument("point transfer scalar/vector layout differs across leaves");
    std::optional<Numeric> mass;
    if(!o.weightAttribute.empty()) {
      if(!leaf->hasAttribute(o.weightAttribute))throw std::invalid_argument("point transfer weight attribute was not found");
      mass.emplace(leaf->constAttributeArray(o.weightAttribute));
      if(mass->components!=1)throw std::invalid_argument("point transfer weight attribute must be scalar");
    }
    Predicate chosen(*leaf,o.groups);points::AttributeHandle<openvdb::Vec3f> positions(leaf->constAttributeArray("P"));
    for(auto i=leaf->beginIndexAll();i;++i)if(chosen(*i)) {
      Record r;r.value=field.get(*i,o.integers,q.result.roundedIntegerComponents);
      r.mass=mass?mass->get(*i,o.integers,q.result.roundedIntegerComponents)[0]:o.constantMass;
      if(!std::isfinite(r.mass) || r.mass<=0)throw std::invalid_argument("point transfer selected mass must be finite and strictly positive");
      const auto index=target.index(source.world(i.getCoord(),positions.get(*i)));
      for(size_t a=0;a<3;++a) {r.base[a]=int32_t(std::floor(index[a]));r.coordinate[a]=index[a];}
      q.totalMass.add(units(binary(r.mass),-1074));
      for(int x=0;x<2;++x)for(int y=0;y<2;++y)for(int z=0;z<2;++z) {
        if((x && r.coordinate[0]==double(r.base[0])) || (y && r.coordinate[1]==double(r.base[1])) || (z && r.coordinate[2]==double(r.base[2])))continue;
        q.field.add(r.base+C(x,y,z));++q.result.contributionCount;
      }
      q.proxy.add(r.base);q.records.push_back(r);++q.result.selectedCount;
      q.result.selectedInactiveCount+=!leaf->isValueOn(i.getCoord());
    }
  }
  if(first) {
    q.result.componentCount=archivedComponents(s,attribute);
    if(!o.weightAttribute.empty() && archivedComponents(s,o.weightAttribute)!=1)
      throw std::invalid_argument("point transfer archived weight must be scalar");
  }
  try{q.result.totalMass=ratio(q.totalMass,Big(1),-1074);}
  catch(const std::invalid_argument&){throw std::invalid_argument("point transfer total selected mass must be representable as Double");}
  const long double slots=s.leaves.size()*512.0L+2*q.field.leaves.size()*512.0L+2*q.proxy.leaves.size()*512.0L;
  if(slots>p.maxVoxelCount || 2*(q.field.cells.size()+q.proxy.cells.size())>p.maxVoxelCount)
    throw std::invalid_argument("point transfer joint source/output/proxy stored topology exceeds maxVoxelCount");
  const long double outputMetadata=2*(metadataBytes(*s.grid)+4096.0L)+metadataBytes(*s.fileMetadata);
  if(s.metadata+outputMetadata>o.maxMetadataBytes)throw std::invalid_argument("point transfer joint source/output metadata exceeds maxMetadataBytes");
  const long double trees=treeBytes<openvdb::Vec3DTree>(q.field)+treeBytes<openvdb::DoubleTree>(q.field)+
    treeBytes<Tree>(q.proxy)+treeBytes<openvdb::tools::PointIndexTree>(q.proxy);
  // Worst-case fixed-limb products, exact rational54-bit division, eight-cell
  // stamps, and27 neighboring512-voxel native source buckets per output leaf.
  q.work+=2*q.records.size()*8.0L*(80000+8000*q.result.componentCount)+
    q.field.cells.size()*(120000.0L*(q.result.componentCount+1)+10000)+
    (q.field.slots()+q.proxy.slots())*32+2*q.field.leaves.size()*27.0L*512+
    2.0L*q.field.leaves.size()*sizeof(LeafScratch)/sizeof(uint32_t)+8*outputMetadata;
  if(q.work>o.maxWork)throw std::invalid_argument("point transfer exact arithmetic/native traversal exceeds maxWork");
  q.reserve=s.resident+16*trees+2.0L*sizeof(LeafScratch)+q.field.leaves.size()*sizeof(LeafResult)*8.0L+
    q.records.size()*2048.0L+(q.field.cells.size()+q.proxy.cells.size())*128.0L+16*outputMetadata+Fixed;
  budget(q.reserve,p,"point transfer joint trees/arrays/exact scratch/writer exceeds shared memory budget");
  std::map<C,uint64_t> pointsPerLeaf;for(const auto& r:q.records)
    if(++pointsPerLeaf[r.base&~7]>uint64_t(INT32_MAX))throw std::invalid_argument("point transfer proxy leaf exceeds native Int32 point offsets");
  return q;
}
Grid::Ptr proxy(const Plan& q) {
  std::vector<V> positions;std::vector<int64_t> references;
  positions.reserve(q.records.size());references.reserve(q.records.size());
  for(size_t i=0;i<q.records.size();++i){positions.push_back(q.records[i].base.asVec3d());references.push_back(int64_t(i));}
  auto unit=openvdb::math::Transform::createLinearTransform(1);
  points::PointAttributeVector<V> wrapper(positions);
  auto indices=openvdb::tools::createPointIndexGrid<openvdb::tools::PointIndexGrid>(wrapper,*unit);
  auto result=points::createPointDataGrid<points::NullCodec,Grid>(*indices,wrapper,*unit);
  points::appendAttribute<int64_t>(result->tree(),"ordinal");
  points::PointAttributeVector<int64_t> ids(references);points::populateAttribute(result->tree(),indices->tree(),"ordinal",ids);
  std::vector<uint8_t> seen(q.records.size());uint64_t count=0;
  for(auto leaf=result->tree().cbeginLeaf();leaf;++leaf) {
    points::AttributeHandle<int64_t> ids(leaf->constAttributeArray("ordinal"));
    for(auto i=leaf->beginIndexAll();i;++i) {
      const auto ordinal=ids.get(*i);
      if(ordinal<0 || uint64_t(ordinal)>=q.records.size() || seen[size_t(ordinal)]++ || q.records[size_t(ordinal)].base!=i.getCoord())
        throw std::runtime_error("native point transfer proxy lost, duplicated or misplaced original record references");
      ++count;
    }
  }
  if(count!=q.records.size() || result->tree().leafCount()!=q.proxy.leaves.size())throw std::runtime_error("native point transfer proxy lost records or changed planned buckets");
  return result;
}
template<class FieldTree>struct Transfer:public points::VolumeTransfer<FieldTree,openvdb::DoubleTree> {
  using Base=points::VolumeTransfer<FieldTree,openvdb::DoubleTree>;
  using Value=typename FieldTree::ValueType;
  const Plan* plan;
  const PointTransferOptions* options;
  std::vector<LeafResult>* results;
  // All SDK functor copies BORROW one centrally owned heap block. The whole
  // API runs in task_arena(1), callbacks are nonrecursive and never launch
  // nested parallel work: initialize->stamp->finalize completes before reuse.
  // Thus functor retention/copy lifetimes cannot multiply exact scratch.
  LeafScratch* scratch;
  std::unique_ptr<points::AttributeHandle<int64_t>> ids;
  size_t current=0;
  bool verifyOnly=false;
  Transfer(FieldTree& field,openvdb::DoubleTree& weights,const Plan& q,const PointTransferOptions& o,std::vector<LeafResult>& r,LeafScratch& shared,bool verify)
    :Base(field,weights),plan(&q),options(&o),results(&r),scratch(&shared),verifyOnly(verify){}
  Transfer(const Transfer& t):Base(t),plan(t.plan),options(t.options),results(t.results),scratch(t.scratch),verifyOnly(t.verifyOnly){}
  openvdb::Int32 range(const C&,size_t)const{return 1;}
  void initialize(const C& origin,size_t index,const openvdb::CoordBBox& bounds) {
    Base::initialize(origin,index,bounds);current=index;(*results)[current]={};
    std::fill(scratch->values.begin(),scratch->values.end(),Accumulator{});
  }
  bool startPointLeaf(const Leaf& leaf){ids=std::make_unique<points::AttributeHandle<int64_t>>(leaf.constAttributeArray("ordinal"));return true;}
  bool endPointLeaf(const Leaf&){return true;}
  void rasterizePoint(const C& cell,openvdb::Index id,const openvdb::CoordBBox& bounds) {
    const auto ordinal=ids->get(id);
    if(ordinal<0 || uint64_t(ordinal)>=plan->records.size())throw std::runtime_error("native point transfer proxy has invalid original ordinal");
    const auto& record=plan->records[size_t(ordinal)];
    if(record.base!=cell)throw std::runtime_error("native point transfer proxy changed integer bucket coordinates");
    std::array<std::array<Dyad,2>,3> factors;
    for(size_t a=0;a<3;++a)factors[a]=axisFactors(record.coordinate[a],record.base[a]);
    const auto mass=binary(record.mass);
    for(int x=0;x<2;++x)for(int y=0;y<2;++y)for(int z=0;z<2;++z) {
      const C coordinate=record.base+C(x,y,z);
      if(!bounds.isInside(coordinate) || factors[0][x].magnitude.zero() || factors[1][y].magnitude.zero() || factors[2][z].magnitude.zero())continue;
      const auto offset=FieldTree::LeafNodeType::coordToOffset(coordinate);
      if(!this->template mask<0>()->isOn(offset))throw std::runtime_error("point transfer native support differs from preflight");
      const auto weight=multiply(multiply(multiply(factors[0][x],factors[1][y]),factors[2][z]),mass);
      auto& accum=scratch->values[offset];accum.weight.add(units(weight,-4296));
      if(!verifyOnly)for(size_t a=0;a<plan->result.componentCount;++a){const auto term=multiply(weight,binary(record.value[a]));accum.numerator[a].add(units(term,-5370),term.negative);}
      ++(*results)[current].contributions;
    }
  }
  bool finalize(const C&,size_t) {
    auto& result=(*results)[current];
    const auto volume=binary(plan->result.voxelVolume);const Big volumeUnits=units(volume,-1074);
    for(auto i=this->template mask<0>()->beginOn();i;++i) {
      const auto offset=i.pos();const auto& accum=scratch->values[offset];
      if(accum.weight.zero())throw std::runtime_error("point transfer native positive support received no mass");
      result.canonicalWeight.add(accum.weight);
      if(verifyOnly)continue;
      std::array<double,3> value{};
      for(size_t a=0;a<plan->result.componentCount;++a) {
        value[a]=options->mode==PointTransferMode::WeightedSum?signedRatio(accum.numerator[a],Big(1),-5370):signedRatio(accum.numerator[a],accum.weight,-1074);
        if(options->storage==PointTransferStorage::Float) {
          if(std::abs(value[a])>std::numeric_limits<float>::max())throw std::invalid_argument("point transfer Float field conversion overflows");
          const float f=float(value[a]);
          if(!std::isfinite(f) || (value[a]!=0 && f==0))throw std::invalid_argument("point transfer Float field conversion overflows or underflows");
          if(double(f)!=value[a]){++result.roundedFloat;result.floatError=std::max(result.floatError,std::abs(double(f)-value[a]));}value[a]=f;
        }
        if(!result.hasValues){result.minimum[a]=result.maximum[a]=value[a];}
        else{result.minimum[a]=std::min(result.minimum[a],value[a]);result.maximum[a]=std::max(result.maximum[a],value[a]);}
      }
      result.hasValues=true;
      if constexpr(openvdb::VecTraits<Value>::IsVec)this->template buffer<0>()[offset]=Value(value[0],value[1],value[2]);
      else this->template buffer<0>()[offset]=Value(value[0]);
      const double companion=options->companion==PointTransferCompanion::WeightSum?ratio(accum.weight,Big(1),-4296):ratio(accum.weight,volumeUnits,-3222);
      this->template buffer<1>()[offset]=companion;
      auto integral=binary(companion);
      if(options->companion==PointTransferCompanion::WorldDensity)integral=multiply(integral,volume);
      result.roundedIntegral.add(units(integral,options->companion==PointTransferCompanion::WeightSum?-1074:-2148));
    }
    return true;
  }
};

template<class T>class ExactWriteGrid:public T {
public:
  explicit ExactWriteGrid(const T& grid):T(openvdb::ConstPtrCast<typename T::TreeType>(grid.constTreePtr())) {
    this->insertMeta(grid);this->setTransform(detail::CopyTransformExact(grid.transform()));
  }
  openvdb::GridBase::ConstPtr copyGrid()const override{return this->copyReplacingMetadataAndTransform(*this,detail::CopyTransformExact(this->transform()));}
  openvdb::GridBase::Ptr copyGrid()override{return openvdb::ConstPtrCast<openvdb::GridBase>(static_cast<const ExactWriteGrid&>(*this).copyGrid());}
};
void publish(const std::filesystem::path& source,const std::filesystem::path& output) {
  bool owned=false;std::ofstream target;
  try {
    std::ifstream input(source,std::ios::binary);if(!input)throw std::runtime_error("point transfer cannot read completed output");
    target.open(output,std::ios::binary|std::ios::out|std::ios::noreplace);owned=target.is_open();
    if(!owned)throw std::runtime_error("point transfer cannot exclusively create output");
    target.exceptions(std::ios::badbit|std::ios::failbit);input.exceptions(std::ios::badbit);
    std::array<char,65536> buffer{};
    while(input.read(buffer.data(),std::streamsize(buffer.size())) || input.gcount())target.write(buffer.data(),input.gcount());
    if(!input.eof())throw std::runtime_error("point transfer completed output read failed");target.close();
  }catch(...) {
    target.exceptions(std::ios::goodbit);if(target.is_open())target.close();
    if(owned){std::error_code e;std::filesystem::remove(output,e);}throw;
  }
}
void metadata(openvdb::GridBase& grid,const Source& s,const Plan& q,const std::string& attribute,const PointTransferOptions& o,const std::string& name,bool weight) {
  grid.insertMeta(*s.grid);
  detail::RemoveBoundaryProjectionMetadata(grid);
  // Copied reserved keys may have foreign types. Remove them before typed
  // setters, and discard source topology/solver semantics on these new fields.
  for(const auto* key:{openvdb::GridBase::META_GRID_NAME,openvdb::GridBase::META_GRID_CLASS,
      openvdb::GridBase::META_VECTOR_TYPE,openvdb::GridBase::META_IS_LOCAL_SPACE,
      openvdb::GridBase::META_SAVE_HALF_FLOAT,openvdb::GridBase::META_FILE_BBOX_MIN,
      openvdb::GridBase::META_FILE_BBOX_MAX,openvdb::GridBase::META_FILE_MEM_BYTES,
      openvdb::GridBase::META_FILE_VOXEL_COUNT,openvdb::GridBase::META_FILE_COMPRESSION,
      openvdb::GridBase::META_FILE_DELAYED_LOAD})grid.removeMeta(key);
  for(const auto* key:ArchiveKeys)grid.removeMeta(key);
  for(const char* key:{"container:mac_layout","container:velocity_units",
      "container:pressure_units","container:pressure_gauge","container:projection_dt",
      "container:potential_flow_units","container:potential_flow_boundary",
      "container:surface_field_units","container:surface_field_validity",
      "container:surface_field_location"})grid.removeMeta(key);
  grid.removeMeta("container:point_count");grid.setName(name);grid.setGridClass(openvdb::GRID_UNKNOWN);grid.setIsInWorldSpace(true);grid.setVectorType(openvdb::VEC_INVARIANT);grid.setSaveFloatAsHalf(false);
  for(const char* key:{"container:point_transfer","container:source_point_attribute","container:source_point_count","container:selected_point_count","container:point_transfer_voxel_volume"})grid.removeMeta(key);
  grid.insertMeta("container:point_transfer",openvdb::StringMetadata(weight?(o.companion==PointTransferCompanion::WeightSum?"CIC positive weight sum":"CIC world density"):(o.mode==PointTransferMode::WeightedMean?"CIC stored-value weighted mean":"CIC stored-value weighted sum")));
  grid.insertMeta("container:source_point_attribute",openvdb::StringMetadata(attribute));
  grid.insertMeta("container:source_point_count",openvdb::Int64Metadata(int64_t(q.result.inputCount)));
  grid.insertMeta("container:selected_point_count",openvdb::Int64Metadata(int64_t(q.result.selectedCount)));
  grid.insertMeta("container:point_transfer_voxel_volume",openvdb::DoubleMetadata(q.result.voxelVolume));
  grid.setTransform(detail::CopyTransformExact(*q.transform));
}
void canonical(const Plan& q,const std::vector<LeafResult>& results) {
  Big actual;uint64_t count=0;
  for(const auto& r:results){actual.add(r.canonicalWeight);count+=r.contributions;}
  auto expected=q.totalMass;expected.shift(3222);
  if(count!=q.result.contributionCount || expected.compare(actual)!=0)
    throw std::runtime_error("native point transfer did not conserve exact canonical CIC mass/contributions");
}
template<class G>PointTransferResult execute(const Source& source,Plan& q,const std::filesystem::path& output,const std::string& attribute,const PointTransferOptions& o,const ProcessingOptions& p) {
  auto field=G::create(openvdb::zeroVal<typename G::ValueType>());
  auto weights=openvdb::DoubleGrid::create(0.);
  for(const auto& cell:q.field.cells){field->tree().setValueOn(cell,openvdb::zeroVal<typename G::ValueType>());weights->tree().setValueOn(cell,0.);}
  std::vector<LeafResult> results(q.field.leaves.size());
  if(!q.records.empty()) {
    auto points=proxy(q);
    auto scratch=std::make_unique<LeafScratch>();
    // Verify exact mass with a native traversal BEFORE converting any cell's
    // density, Float field or Double field. A second bounded traversal writes.
    {Transfer<typename G::TreeType> transfer(field->tree(),weights->tree(),q,o,results,*scratch,true);points::rasterize(*points,transfer);}
    canonical(q,results);
    {Transfer<typename G::TreeType> transfer(field->tree(),weights->tree(),q,o,results,*scratch,false);points::rasterize(*points,transfer);}
    canonical(q,results);
  }
  Big integral;
  for(const auto& r:results) {
    integral.add(r.roundedIntegral);q.result.roundedFloatComponents+=r.roundedFloat;
    q.result.maxFloatAbsoluteError=std::max(q.result.maxFloatAbsoluteError,r.floatError);
    if(r.hasValues)for(size_t a=0;a<q.result.componentCount;++a) {
      if(!q.result.hasValues){q.result.minimum[a]=r.minimum[a];q.result.maximum[a]=r.maximum[a];}
      else{q.result.minimum[a]=std::min(q.result.minimum[a],r.minimum[a]);q.result.maximum[a]=std::max(q.result.maximum[a],r.maximum[a]);}
    }
    q.result.hasValues|=r.hasValues;
  }
  const int unit=o.companion==PointTransferCompanion::WeightSum?-1074:-2148;
  try{q.result.depositedMass=ratio(integral,Big(1),unit);}
  catch(const std::invalid_argument&){throw std::invalid_argument("point transfer integrated native companion mass must be representable as Double");}
  auto expected=q.totalMass;if(unit==-2148)expected.shift(1074);
  auto error=integral;
  if(error.compare(expected)>=0)error.subtract(expected);else{error=expected;error.subtract(integral);}
  q.result.massErrorUpperBound=ratio(error,Big(1),unit,true);
  const auto companionName=o.companionGridName.empty()?p.gridName+(o.companion==PointTransferCompanion::WeightSum?"_weight":"_density"):o.companionGridName;
  metadata(*field,source,q,attribute,o,p.gridName,false);metadata(*weights,source,q,attribute,o,companionName,true);
  if(field->tree().leafCount()!=q.field.leaves.size() || weights->tree().leafCount()!=q.field.leaves.size() || field->activeVoxelCount()!=q.field.cells.size() || weights->activeVoxelCount()!=q.field.cells.size())
    throw std::runtime_error("native point transfer changed planned positive-support output topology");
  const double h=std::hypot(q.transform->baseMap()->applyJacobian({1,0,0})[0],q.transform->baseMap()->applyJacobian({1,0,0})[1],q.transform->baseMap()->applyJacobian({1,0,0})[2]);
  q.result.field={p.gridName,"unknown",field->activeVoxelCount(),h};q.result.companion={companionName,"unknown",weights->activeVoxelCount(),h};
  q.result.workUpperBound=uint64_t(std::ceil(q.work));
  q.result.diagnostics.emplace_back("Native PointTransfer uses cached Double CIC coordinates and exact dyadic accumulation; stored payload values receive no implicit frame transformation.");
  q.result.diagnostics.emplace_back("Positive mass defines shared activity; zero-valued or exactly cancelled fields stay active. Native companion rounding residual is reported separately.");
  if(q.result.roundedIntegerComponents)q.result.diagnostics.emplace_back("Explicit AllowRounded converted inexact native Int64 field/weight components to Double; roundedIntegerComponents counts the conversions.");
  if(q.result.roundedFloatComponents)q.result.diagnostics.emplace_back("Explicit Float output rounded representable field components; maxFloatAbsoluteError records the largest native conversion error.");
  const long double finalBytes=openvdb::tools::memUsageIfLoaded(field->tree(),false)+openvdb::tools::memUsageIfLoaded(weights->tree(),false)+8*(metadataBytes(*field)+metadataBytes(*weights));
  budget(q.reserve+8*finalBytes+Fixed,p,"point transfer final native outputs/writer exceed shared memory budget");
  const auto temporary=source.snapshot->directory/"transferred.vdb";
  struct Cleanup {std::filesystem::path file;~Cleanup(){std::error_code e;std::filesystem::remove(file,e);}}cleanup{temporary};
  openvdb::GridPtrVec grids{std::make_shared<ExactWriteGrid<G>>(*field),std::make_shared<ExactWriteGrid<openvdb::DoubleGrid>>(*weights)};
  openvdb::io::File writer(temporary.string());writer.setCompression(openvdb::io::COMPRESS_ZIP);writer.write(grids,*source.fileMetadata);writer.close();
  budget(q.reserve+8*finalBytes+std::filesystem::file_size(temporary)+Fixed,p,"point transfer encoded native output exceeds shared memory budget");
  static_assert(std::is_nothrow_move_constructible_v<PointTransferResult>);publish(temporary,output);return std::move(q.result);
}
#endif
} // namespace

PointTransferResult WriteTransferredPointAttribute(const std::filesystem::path& input,const std::filesystem::path& output,const std::string& attribute,const PointTransferOptions& o,const std::string& selector,const ProcessingOptions& p,const PointAttributeOptions& limits) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  validateOptions(p,limits,true);name(attribute);matrix(o.targetIndexToWorld);
  if(attribute=="P" || o.weightAttribute=="P")throw std::invalid_argument("point transfer P is a coordinate, not a deposited numeric attribute");
  if(o.mode<PointTransferMode::WeightedSum || o.mode>PointTransferMode::WeightedMean ||
     o.storage<PointTransferStorage::Double || o.storage>PointTransferStorage::Float ||
     o.companion<PointTransferCompanion::WeightSum || o.companion>PointTransferCompanion::WorldDensity ||
     o.integers<PointTransferIntegers::RejectInexact || o.integers>PointTransferIntegers::AllowRounded ||
     !o.maxWork || o.maxWork>10'000'000'000ULL || !o.maxGrids || o.maxGrids>4096 || !o.maxMetadataBytes ||
     !std::isfinite(o.constantMass) || o.constantMass<=0 || (!o.weightAttribute.empty() && o.constantMass!=1))
    throw std::invalid_argument("point transfer modes/counts/mass controls are invalid or irrelevant");
  if(!o.weightAttribute.empty())name(o.weightAttribute);
  const auto companionName=o.companionGridName.empty()?p.gridName+(o.companion==PointTransferCompanion::WeightSum?"_weight":"_density"):o.companionGridName;
  name(companionName);if(companionName==p.gridName)throw std::invalid_argument("point transfer field and companion names must differ");
  size_t terms=0;for(const auto* names:{&o.groups.anyOf,&o.groups.allOf,&o.groups.noneOf})for(const auto& n:*names) {
    name(n);if(++terms>3*limits.maxGroups)throw std::invalid_argument("point transfer group predicate exceeds count cap");
  }
  if(output.empty() || std::filesystem::exists(output) || !std::filesystem::is_directory(output.has_parent_path()?output.parent_path():std::filesystem::path(".")))
    throw std::invalid_argument("point transfer output must be new in an existing directory");
  static std::once_flag once;std::call_once(once,[]{initialize();registerType<openvdb::Vec3i>();});
  tbb::task_arena arena(1);return arena.execute([&] {
    const auto source=loadSource(input,selector,o,p,limits);auto q=plan(source,attribute,o,p);
    if(q.result.componentCount==1) {
      if(o.storage==PointTransferStorage::Double)return execute<openvdb::DoubleGrid>(source,q,output,attribute,o,p);
      return execute<openvdb::FloatGrid>(source,q,output,attribute,o,p);
    }
    if(o.storage==PointTransferStorage::Double)return execute<openvdb::Vec3DGrid>(source,q,output,attribute,o,p);
    return execute<openvdb::Vec3SGrid>(source,q,output,attribute,o,p);
  });
#else
  (void)input;(void)output;(void)attribute;(void)o;(void)selector;(void)p;(void)limits;unavailable();
#endif
}
} // namespace container::geometry::vdb
