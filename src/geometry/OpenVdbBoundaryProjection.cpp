#include "Container/geometry/OpenVdbBoundaryProjection.h"
#include "Container/geometry/OpenVdbGridSelection.h"
#include <stdexcept>
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
#include "OpenVdbFileUtils.h"
#include <algorithm>
#include <bit>
#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <openvdb/io/GridDescriptor.h>
#include <openvdb/math/ConjGradient.h>
#include <openvdb/tools/Count.h>
#include <openvdb/util/NullInterrupter.h>
#include <set>
#include <sstream>
#include <tbb/task_arena.h>
#include <type_traits>
#endif
namespace container::geometry::vdb {
namespace {
[[noreturn]] void unavailable(){throw std::runtime_error("OpenVDB boundary projection is unavailable; configure with -DENABLE_OPENVDB=ON.");}
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
using V=openvdb::Vec3d;using C=openvdb::Coord;
using Mac=openvdb::Vec3DGrid;using Scalar=openvdb::DoubleGrid;using Mask=openvdb::BoolGrid;
using Matrix=openvdb::math::pcg::SparseStencilMatrix<double,7>;
using Vector=openvdb::math::pcg::Vector<double>;
constexpr long double Fixed=2*1024*1024;
constexpr const char* Schema="container:boundary_projection_schema";
constexpr const char* Layout="container:boundary_face_channels";
constexpr const char* Version="negative_xyz_faces_v1";
void budget(long double bytes,const ProcessingOptions& p,const char* message){if(!std::isfinite(bytes)||bytes>p.maxInputBytes)throw std::invalid_argument(message);}
long double metadata(const openvdb::MetaMap& map){long double n=sizeof(map);for(auto i=map.beginMeta();i!=map.endMeta();++i)n+=256+8.0L*(i->first.size()+i->second->typeName().size()+i->second->size());return n;}
double gap(double x){return std::max(std::nextafter(x,INFINITY)-x,x-std::nextafter(x,-INFINITY));}
double norm(const V& v){return std::max({std::abs(v[0]),std::abs(v[1]),std::abs(v[2])});}
struct Sum {
  double value=0,correction=0,magnitude=0;
  void add(double x){double n=value+x;correction+=std::abs(value)>=std::abs(x)?(value-n)+x:(x-n)+value;value=n;magnitude+=std::abs(x);}
  double get()const{double x=value+correction;if(!std::isfinite(x))throw std::invalid_argument("boundary projection compensated sum exceeds finite Double range");return x;}
  void product(double a,double b){double x=a*b;if(!std::isfinite(x)||(a!=0&&b!=0&&x==0))throw std::invalid_argument("boundary projection coordinate product overflows/underflows");add(x);add(std::fma(a,b,-x));}
  void scaledProduct(double a,double b,double c){
    if(a==0||b==0||c==0)return;int ea,eb,ec;const double ma=std::frexp(a,&ea),mb=std::frexp(b,&eb),mc=std::frexp(c,&ec);
    const double high=ma*mb,low=std::fma(ma,mb,-high),top=high*mc,tail=std::fma(high,mc,-top),bottom=low*mc,last=std::fma(low,mc,-bottom);
    // TwoProduct decompositions retain cancellation-sensitive residuals before
    // world-frame terms are summed. Normalize first so dt can rescue a tiny
    // product; unresolved nonzero scaled residuals reject conservatively.
    for(double term:{top,tail,bottom,last})if(term!=0){const double x=std::ldexp(term,ea+eb+ec);if(!std::isfinite(x)||x==0)throw std::invalid_argument("boundary projection WORLD frame product/residual overflows/underflows");add(x);}
  }
  double error()const{return 64*std::numeric_limits<double>::epsilon()*std::numeric_limits<double>::epsilon()*magnitude+gap(get());}
};
double scale(double value,std::initializer_list<double> factors,std::initializer_list<double> divisors={}) {
  if(value==0)return 0;int exponent=0;double mantissa=std::frexp(value,&exponent);
  for(double f:factors){if(f==0)return 0;int e;mantissa*=std::frexp(f,&e);exponent+=e;}
  for(double d:divisors){if(d==0)throw std::invalid_argument("boundary projection zero scale divisor");int e;mantissa/=std::frexp(d,&e);exponent-=e;}
  double result=std::ldexp(mantissa,exponent);
  if(!std::isfinite(result)||result==0)throw std::invalid_argument("boundary projection nonzero physical quantity overflows/underflows Double");return result;
}
double difference(double a,double b){Sum s;s.add(a);s.add(-b);return s.get();}
bool same(const openvdb::math::Transform& a,const openvdb::math::Transform& b){auto x=a.baseMap(),y=b.baseMap();if(x->applyMap(V(0))!=y->applyMap(V(0)))return false;for(const V e:{V(1,0,0),V(0,1,0),V(0,0,1)})if(x->applyJacobian(e)!=y->applyJacobian(e))return false;return true;}
// Exact identity of two finite Double products, without forming a product
// in Double. Normalized significands are odd and <=53 bits, so their product
// needs at most106 bits. Two UInt64 words plus exponent/sign are sufficient;
// all 32-bit partial products/carries below fit UInt64. No heap or native
// extended-precision assumption is involved, even for products below2^-1074.
struct ProductKey {
  uint64_t low=0,high=0;int unit=0;bool negative=false;
  bool operator==(const ProductKey&)const=default;
};
ProductKey exactProductKey(double a,double b) {
  static_assert(std::numeric_limits<double>::is_iec559 && std::numeric_limits<double>::digits==53 &&
                std::numeric_limits<double>::max_exponent==1024 && std::numeric_limits<double>::min_exponent==-1021);
  struct Factor {uint64_t significand;int unit;bool negative;};
  const auto factor=[](double value) {
    const auto bits=std::bit_cast<uint64_t>(value),exponent=(bits>>52)&2047;
    uint64_t sig=(bits&UINT64_C(0xfffffffffffff))|(exponent?UINT64_C(0x10000000000000):0);
    if(!sig)return Factor{0,0,false};
    const int zeros=std::countr_zero(sig);
    return Factor{sig>>zeros,(exponent?int(exponent)-1075:-1074)+zeros,bool(bits>>63)};
  };
  const auto x=factor(a),y=factor(b);
  if(!x.significand || !y.significand)return {};
  const uint64_t a0=uint32_t(x.significand),a1=x.significand>>32,
                 b0=uint32_t(y.significand),b1=y.significand>>32;
  const uint64_t w0=a0*b0,t=a1*b0+(w0>>32),w2=t>>32,
                 w1=uint64_t(uint32_t(t))+a0*b1;
  return {(w1<<32)|uint32_t(w0),a1*b1+w2+(w1>>32),x.unit+y.unit,x.negative!=y.negative};
}
struct Affine {
  V origin;std::array<V,3> axes,inverse;double h=0,condition=0,inverseNorm=0;
  explicit Affine(const openvdb::math::Transform& t,bool uniform=false) {
    if(!t.isLinear())throw std::invalid_argument("boundary projection requires affine maps");auto m=t.baseMap();origin=m->applyMap(V(0));
    std::array<V,3> unit;double lengths[3];
    for(int a=0;a<3;++a){V e(0);e[a]=1;axes[a]=m->applyJacobian(e);lengths[a]=std::hypot(axes[a][0],axes[a][1],axes[a][2]);if(!std::isfinite(origin[a])||!std::isfinite(lengths[a])||lengths[a]<1e-6||lengths[a]>1e6)throw std::invalid_argument("boundary projection map axes must span[1e-6,1e6]");unit[a]=axes[a]/lengths[a];}
    h=lengths[0];double det=unit[0].dot(unit[1].cross(unit[2]));if(!std::isfinite(det)||std::abs(det)<1e-8)throw std::invalid_argument("boundary projection map is singular/ill-conditioned");
    inverse={unit[1].cross(unit[2])/(det*lengths[0]),unit[2].cross(unit[0])/(det*lengths[1]),unit[0].cross(unit[1])/(det*lengths[2])};
    for(int a=0;a<3;++a)for(int b=0;b<3;++b){
      const int c=(a+1)%3,d=(a+2)%3,j=(b+1)%3,k=(b+2)%3;
      const bool zero=exactProductKey(axes[c][j],axes[d][k])==exactProductKey(axes[c][k],axes[d][j]);
      if(!std::isfinite(inverse[a][b]) || zero!=(inverse[a][b]==0))
        throw std::invalid_argument("boundary projection Double inverse coefficient loses a nonzero represented-map cofactor or introduces a spurious component; condition the map");
    }
    double forwardNorm=0;
    for(int a=0;a<3;++a){forwardNorm=std::max(forwardNorm,std::abs(axes[0][a])+std::abs(axes[1][a])+std::abs(axes[2][a]));inverseNorm=std::max(inverseNorm,std::abs(inverse[a][0])+std::abs(inverse[a][1])+std::abs(inverse[a][2]));}
    condition=forwardNorm*inverseNorm;if(!std::isfinite(condition)||condition>1e8)throw std::invalid_argument("boundary projection inverse map condition exceeds1e8");
    if(uniform&&(std::abs(lengths[1]/h-1)>1e-9||std::abs(lengths[2]/h-1)>1e-9||std::abs(unit[0].dot(unit[1]))>1e-9||std::abs(unit[0].dot(unit[2]))>1e-9||std::abs(unit[1].dot(unit[2]))>1e-9))throw std::invalid_argument("boundary projection fluid map must be uniform orthogonal");
  }
  V vector(const V& v,double dt=1)const{V r;for(int a=0;a<3;++a){Sum s;for(int b=0;b<3;++b)s.scaledProduct(v[b],inverse[a][b],dt);r[a]=s.get();}return r;}
  V world(const V& q)const{V r;for(int a=0;a<3;++a){Sum s;s.add(origin[a]);for(int b=0;b<3;++b)s.product(axes[b][a],q[b]);r[a]=s.get();if(gap(r[a])>h*1e-4)throw std::invalid_argument("boundary projection world face precision exceeds1e-4 voxel");}return r;}
};
struct SamplingMap {
  V offset;std::array<V,3> columns;double error=0,condition=0;
  SamplingMap(const Affine& domain,const Affine& source){V delta;double bound=0;for(int a=0;a<3;++a){Sum s;s.add(domain.origin[a]);s.add(-source.origin[a]);delta[a]=s.get();bound=std::max(bound,s.error());}
    for(int a=0;a<3;++a){Sum s;for(int b=0;b<3;++b)s.product(source.inverse[a][b],delta[b]);offset[a]=s.get();for(int c=0;c<3;++c){Sum v;for(int b=0;b<3;++b)v.product(source.inverse[a][b],domain.axes[c][b]);columns[c][a]=v.get();}}
    error=bound*source.inverseNorm;condition=domain.condition*source.condition;}
  V at(const V& q)const{V r;double envelope=std::max(1.,norm(offset));for(int a=0;a<3;++a){Sum s;s.add(offset[a]);double terms=std::abs(offset[a]);for(int b=0;b<3;++b){s.product(columns[b][a],q[b]);terms+=std::abs(columns[b][a]*q[b]);}r[a]=s.get();envelope=std::max(envelope,terms);if(s.error()>1e-4)throw std::invalid_argument("boundary projection sampling coordinate loses precision");}
    if(!std::isfinite(error)||error+256*std::numeric_limits<double>::epsilon()*condition*envelope>1e-4)throw std::invalid_argument("boundary projection sampling inverse uncertainty exceeds1e-4 voxel");
    for(int a=0;a<3;++a)if(!std::isfinite(r[a])||r[a]<double(INT32_MIN)+8192||r[a]>double(INT32_MAX)-8192||gap(r[a])>1e-4)throw std::invalid_argument("boundary projection sample exceeds native coordinate precision");return r;}
};
template<class F>decltype(auto) dispatch(openvdb::GridBase& g,F&& fn){
#define TYPE(G) if(g.isType<openvdb::G>())return fn(static_cast<openvdb::G&>(g));
  TYPE(BoolGrid) TYPE(MaskGrid) TYPE(FloatGrid) TYPE(DoubleGrid) TYPE(Vec3IGrid) TYPE(Vec3SGrid) TYPE(Vec3DGrid)
#undef TYPE
  throw std::invalid_argument("boundary projection native type is unsupported");
}
template<class T>bool finite(const T& v,double cap=std::numeric_limits<double>::max()) {if constexpr(std::is_arithmetic_v<T>)return std::isfinite(double(v))&&std::abs(double(v))<=cap;else{for(int a=0;a<3;++a)if(!finite(v[a],cap))return false;return true;}}
template<class G>long double validationWork(const G& g){using Tree=typename G::TreeType;using Upper=typename Tree::RootNodeType::ChildNodeType;using Lower=typename Upper::ChildNodeType;const auto n=g.tree().nodeCount();return g.tree().root().getTableSize()+n[0]*512.0L+n[1]*1.0L*Lower::NUM_VALUES+n[2]*1.0L*Upper::NUM_VALUES;}

struct Snapshot {
  std::filesystem::path directory, file;
  uintmax_t encoded = 0;
  Snapshot(const std::filesystem::path &input, const ProcessingOptions &p) {
    if (!std::filesystem::is_regular_file(input))
      throw std::invalid_argument(
          "boundary projection input is not a regular file");
    encoded = std::filesystem::file_size(input);
    budget(Fixed + static_cast<long double>(encoded), p,
           "boundary projection encoded source exceeds byte cap");
    static std::atomic_uint64_t serial{};
    for (int n = 0; n < 64; ++n) {
      auto candidate =
          std::filesystem::temp_directory_path() /
          ("container-boundary-projection-" +
           std::to_string(
               std::chrono::steady_clock::now().time_since_epoch().count()) +
           "-" + std::to_string(++serial));
      if (std::filesystem::create_directory(candidate)) {
        directory = std::move(candidate);
        file = directory / "source.vdb";
        break;
      }
    }
    if (file.empty())
      throw std::runtime_error("cannot reserve boundary projection snapshot");
    try {
      std::ifstream source(input, std::ios::binary);
      std::ofstream target(file, std::ios::binary | std::ios::out |
                                     std::ios::noreplace);
      if (!source || !target)
        throw std::runtime_error("cannot open boundary projection snapshot");
      target.exceptions(std::ios::badbit | std::ios::failbit);
      std::array<char, 65536> buffer{};
      auto left = encoded;
      while (left) {
        const auto n =
            std::streamsize(std::min<uintmax_t>(left, buffer.size()));
        source.read(buffer.data(), n);
        if (source.gcount() != n)
          throw std::runtime_error(
              "boundary projection source changed while snapshotting");
        target.write(buffer.data(), n);
        left -= uintmax_t(n);
      }
      if (source.bad() || source.peek() != std::char_traits<char>::eof())
        throw std::runtime_error(
            "boundary projection source grew while snapshotting");
      target.close();
    } catch (...) {
      cleanup();
      throw;
    }
  }
  void cleanup() const noexcept {
    std::error_code e;
    std::filesystem::remove(file, e);
    std::filesystem::remove(directory, e);
  }
  ~Snapshot() { cleanup(); }
};
class Archive : public openvdb::io::Archive {
public:
  void begin(std::istream &s, openvdb::io::StreamMetadata::Ptr &metadata) {
    readHeader(s);
    if (!inputHasGridOffsets() ||
        fileVersion() < openvdb::OPENVDB_FILE_VERSION_GRID_INSTANCING ||
        fileVersion() > openvdb::OPENVDB_FILE_VERSION)
      throw std::invalid_argument(
          "boundary projection requires supported seekable VDB offsets");
    openvdb::io::setStreamMetadataPtr(s, metadata, false);
    setFormatVersion(s);
    setLibraryVersion(s);
    setDataCompression(s);
  }
  int32_t count(std::istream &s) { return readGridCount(s); }
  openvdb::GridBase::Ptr next(std::istream &s, openvdb::io::GridDescriptor &d,
                              uintmax_t bytes) {
    auto grid = d.read(s);
    const auto after = int64_t(s.tellg()), start = d.getGridPos(),
               end = d.getEndPos(), blocks = d.getBlockPos();
    if (!grid || start < after || end <= start || uintmax_t(end) > bytes ||
        (!d.isInstance() && (blocks < start || blocks > end)))
      throw std::invalid_argument(
          "boundary projection descriptor offsets are outside encoded source");
    d.seekToGrid(s);
    readGridCompression(s);
    grid->readMeta(s);
    grid->readTransform(s);
    if (fileVersion() < openvdb::OPENVDB_FILE_VERSION_NO_GRIDMAP &&
        grid->getName().empty())
      grid->setName(d.gridName());
    if (int64_t(s.tellg()) > (d.isInstance() ? end : blocks))
      throw std::invalid_argument(
          "boundary projection metadata crosses payload offset");
    grid->removeMeta(openvdb::GridBase::META_FILE_DELAYED_LOAD);
    d.seekToEnd(s);
    return grid;
  }
};
enum class Role { Mask, Velocity, Scalar, FaceKind, FaceValues };
bool accepts(const openvdb::GridBase& g,Role role){
  if(role==Role::Mask)return g.isType<openvdb::BoolGrid>()||g.isType<openvdb::MaskGrid>()||g.isType<openvdb::FloatGrid>()||g.isType<openvdb::DoubleGrid>();
  if(role==Role::Velocity)return g.isType<openvdb::Vec3SGrid>()||g.isType<openvdb::Vec3DGrid>();
  if(role==Role::Scalar)return g.isType<openvdb::FloatGrid>()||g.isType<openvdb::DoubleGrid>();
  return role==Role::FaceKind?g.isType<openvdb::Vec3IGrid>():g.isType<Mac>();
}
struct Bundle {
  std::unique_ptr<Snapshot> snapshot;
  openvdb::MetaMap::Ptr metadata;
  std::vector<std::pair<std::string,openvdb::GridBase::Ptr>> entries;
  std::unique_ptr<openvdb::io::File> file;
};
struct Loader {
  const BoundaryProjectionOptions& o;const ProcessingOptions& p;
  std::map<std::filesystem::path,std::unique_ptr<Bundle>> files;
  std::map<std::pair<Bundle*,std::string>,openvdb::GridBase::Ptr> loaded;
  long double encoded=0,meta=0,full=0,work=0,slots=0;
  Loader(const BoundaryProjectionOptions& options,const ProcessingOptions& limits):o(options),p(limits){
    if(!openvdb::io::Archive::isDelayedLoadingEnabled())throw std::invalid_argument("bounded boundary projection requires delayed loading; unset OPENVDB_DISABLE_DELAYED_LOAD");
#ifndef OPENVDB_USE_DELAYED_LOADING
    throw std::invalid_argument("bounded boundary projection requires delayed-loading SDK support");
#endif
  }
  long double resident()const{return encoded+8*meta+2*full+Fixed;}
  void check()const{if(meta>o.maxMetadataBytes)throw std::invalid_argument("boundary projection aggregate file/grid metadata exceeds cap");budget(resident(),p,"boundary projection aggregate full typed inputs exceed byte cap before paging");if(work>o.maxWorkItems||slots>p.maxVoxelCount)throw std::invalid_argument("boundary projection source traversal/leaf slots exceed work/voxel caps before paging");}
  Bundle& bundle(const std::filesystem::path& input){
    const auto key=std::filesystem::absolute(input).lexically_normal();if(auto i=files.find(key);i!=files.end())return *i->second;
    auto result=std::make_unique<Bundle>();result->snapshot=std::make_unique<Snapshot>(input,p);encoded+=result->snapshot->encoded;check();
    detail::RequireSeekableFile(result->snapshot->file);auto sm=std::make_shared<openvdb::io::StreamMetadata>();sm->setSeekable(true);
    std::ifstream s(result->snapshot->file,std::ios::binary);s.exceptions(std::ios::badbit|std::ios::failbit);Archive archive;archive.begin(s,sm);
    result->metadata=std::make_shared<openvdb::MetaMap>();result->metadata->readMeta(s);meta+=metadata(*result->metadata);check();
    const auto n=archive.count(s);if(n<0||uint64_t(n)>o.maxGrids)throw std::invalid_argument("boundary projection native file catalog exceeds maxGrids");
    std::set<std::string> names;
    for(int32_t i=0;i<n;++i){openvdb::io::GridDescriptor descriptor;auto g=archive.next(s,descriptor,result->snapshot->encoded);auto name=openvdb::io::GridDescriptor::nameAsString(descriptor.uniqueName());if(!names.insert(name).second)throw std::invalid_argument("boundary projection native descriptors collide");meta+=metadata(*g)+4.0L*name.size()+256;check();result->entries.emplace_back(std::move(name),std::move(g));}
    s.close();result->file=std::make_unique<openvdb::io::File>(result->snapshot->file.string());result->file->setCopyMaxBytes(0);result->file->open(true);
    if(result->file->getUniqueTag()!=archive.getUniqueTag())throw std::runtime_error("boundary projection private snapshot changed");
    auto& reference=*result;files.emplace(key,std::move(result));return reference;
  }
  openvdb::GridBase::Ptr get(const BoundaryProjectionField& input,Role role,bool optional=false){
    if(input.file.empty()){if(optional&&input.grid.empty())return {};throw std::invalid_argument("boundary projection binding requires a file");}
    auto& b=bundle(input.file);std::string selected;openvdb::GridBase::Ptr header;
    for(const auto& [name,g]:b.entries){if(!GridSelectionMatches(input.grid,name,g->getName()))continue;if(!accepts(*g,role)){if(!input.grid.empty())throw std::invalid_argument("boundary projection selected grid type is incompatible with role");continue;}if(header){if(input.grid.empty())continue;throw std::invalid_argument("boundary projection grid selector is ambiguous");}header=g;selected=name;}
    if(!header)throw std::invalid_argument("boundary projection selected grid was not found");
    const auto key=std::make_pair(&b,selected);if(auto i=loaded.find(key);i!=loaded.end())return i->second;
    const auto memory=header->getMetadata<openvdb::Int64Metadata>(openvdb::GridBase::META_FILE_MEM_BYTES),count=header->getMetadata<openvdb::Int64Metadata>(openvdb::GridBase::META_FILE_VOXEL_COUNT);
    if(!memory||!count||memory->value()<0||count->value()<0||uint64_t(count->value())>p.maxVoxelCount)throw std::invalid_argument("boundary projection requires valid recorded source memory/count statistics");
    budget(resident()+memory->value(),p,"boundary projection recorded source exceeds memory cap");
    auto grid=b.file->readGrid(selected);if(!same(grid->transform(),header->transform()))throw std::runtime_error("boundary projection native read changed exact map");grid->setTransform(detail::CopyTransformExact(header->transform()));
    long double ownFull=0;dispatch(*grid,[&](const auto& g){ownFull=openvdb::tools::memUsageIfLoaded(g.tree(),false);full+=ownFull;work+=4*validationWork(g);slots+=512.0L*g.tree().leafCount();if(g.activeVoxelCount()>p.maxVoxelCount)throw std::invalid_argument("boundary projection actual source activity exceeds cap");});
    meta+=metadata(*grid);check();grid->readNonresidentBuffers();if(grid->memUsage()>ownFull)throw std::runtime_error("boundary projection paging exceeded full typed source estimate");
    dispatch(*grid,[&](const auto& g){if(!finite(g.background()))throw std::invalid_argument("boundary projection source background is nonfinite");for(auto v=g.cbeginValueAll();v;++v)if(!finite(*v))throw std::invalid_argument("boundary projection stored inactive/active source value is nonfinite");});
    loaded.emplace(key,grid);return grid;
  }
  openvdb::MetaMap::Ptr fileMetadata(const BoundaryProjectionField& binding){return bundle(binding.file).metadata;}
  std::filesystem::path temporary()const{return files.begin()->second->snapshot->directory/"projected.vdb";}
};
void nativeTags(const openvdb::GridBase& g){
  for(const auto* key:{openvdb::GridBase::META_GRID_CLASS,openvdb::GridBase::META_VECTOR_TYPE})if(g[key]&&!g.getMetadata<openvdb::StringMetadata>(key))throw std::invalid_argument("boundary projection native semantic tag has wrong metadata type");
  if(g[openvdb::GridBase::META_IS_LOCAL_SPACE]&&!g.getMetadata<openvdb::BoolMetadata>(openvdb::GridBase::META_IS_LOCAL_SPACE))throw std::invalid_argument("boundary projection space metadata has wrong type");
  if(const auto v=g.getMetadata<openvdb::StringMetadata>(openvdb::GridBase::META_GRID_CLASS);v&&v->value()!=openvdb::GridBase::gridClassToString(g.getGridClass()))throw std::invalid_argument("boundary projection native grid class spelling is unrecognized");
  if(const auto v=g.getMetadata<openvdb::StringMetadata>(openvdb::GridBase::META_VECTOR_TYPE);v&&v->value()!=openvdb::GridBase::vecTypeToString(g.getVectorType()))throw std::invalid_argument("boundary projection native vector semantic spelling is unrecognized");
}
bool selected(const openvdb::GridBase::Ptr& g,const C& c){if(!g)return false;return dispatch(*g,[&](const auto& grid)->bool{using T=typename std::decay_t<decltype(grid)>::ValueType;if constexpr(std::is_arithmetic_v<T>)return grid.tree().isValueOn(c)&&grid.tree().getValue(c)>0;else return false;});}
void mask(const openvdb::GridBase::Ptr& g,const openvdb::math::Transform& t){if(!g)return;if(!same(g->transform(),t))throw std::invalid_argument("boundary projection masks/phi must share exact fluid map");nativeTags(*g);dispatch(*g,[&](const auto& grid){using T=typename std::decay_t<decltype(grid)>::ValueType;if constexpr(std::is_arithmetic_v<T>){if(grid.background()!=0)throw std::invalid_argument("boundary projection mask background must be zero");if constexpr(!std::is_same_v<T,bool>)for(auto v=grid.cbeginValueAll();v;++v)if(*v<0)throw std::invalid_argument("boundary projection mask stored values must be nonnegative");}});}
double scalarAt(const openvdb::GridBase& grid,const C& c){if(grid.isType<openvdb::FloatGrid>())return static_cast<const openvdb::FloatGrid&>(grid).tree().getValue(c);return static_cast<const Scalar&>(grid).tree().getValue(c);}
double lerp(double a,double b,double t){if(t==0)return a;if(t==1)return b;double v=std::lerp(a,b,t);if(!std::isfinite(v)||(v==0&&((a>=0&&b>=0)||(a<=0&&b<=0))&&((a!=0&&1-t!=0)||(b!=0&&t!=0))))throw std::invalid_argument("boundary projection interpolation overflows/underflows");return v;}
template<class Get>double sample(const V& q,Get&& get){const C base=C::floor(q);const V f=q-base.asVec3d();for(int a=0;a<3;++a)if(f[a]<0||f[a]>=1||(f[a]==0&&q[a]!=double(base[a])))throw std::invalid_argument("boundary projection interpolation fraction loses coordinate precision");double yz[2];for(int z=0;z<2;++z){double yv[2];for(int y=0;y<2;++y)yv[y]=lerp(get(base+C(0,y,z)),get(base+C(1,y,z)),f[0]);yz[z]=lerp(yv[0],yv[1],f[1]);}return lerp(yz[0],yz[1],f[2]);}
struct Field {
  openvdb::GridBase::Ptr grid;std::unique_ptr<SamplingMap> mapping;bool mac=false;
  Field()=default;
  Field(openvdb::GridBase::Ptr g,const openvdb::math::Transform& domain,const Affine& frame,bool velocity):grid(std::move(g)){
    if(!grid)return;nativeTags(*grid);Affine source(grid->transform());mac=velocity&&grid->getGridClass()==openvdb::GRID_STAGGERED;
    if(mac){if(grid->isInWorldSpace()||grid->getVectorType()!=openvdb::VEC_CONTRAVARIANT_RELATIVE||!same(grid->transform(),domain))throw std::invalid_argument("boundary projection native MAC requires exact-map local contravariant-relative negative faces");}
    else{if(!grid->isInWorldSpace()||(!velocity&&(grid->getVectorType()!=openvdb::VEC_INVARIANT||grid->getGridClass()==openvdb::GRID_STAGGERED)))throw std::invalid_argument("boundary projection collocated inputs require WORLD scalar/vector semantics");mapping=std::make_unique<SamplingMap>(frame,source);}
    if(velocity)dispatch(*grid,[&](const auto& value){if(!finite(value.background(),mac?1e24:1e12))throw std::invalid_argument("boundary projection velocity background exceeds cap");for(auto i=value.cbeginValueAll();i;++i)if(!finite(*i,mac?1e24:1e12))throw std::invalid_argument("boundary projection stored velocity exceeds cap");});
  }
  V vector(const C& face,int axis,const Affine& frame)const{
    if(mac){if(grid->isType<Mac>())return static_cast<const Mac&>(*grid).tree().getValue(face);return V(static_cast<const openvdb::Vec3SGrid&>(*grid).tree().getValue(face));}
    V q=face.asVec3d();q[axis]-=.5;q=mapping->at(q);V world;
    if(grid->isType<Mac>()){const auto& tree=static_cast<const Mac&>(*grid).tree();for(int a=0;a<3;++a)world[a]=sample(q,[&](const C& c){return tree.getValue(c)[a];});}
    else{const auto& tree=static_cast<const openvdb::Vec3SGrid&>(*grid).tree();for(int a=0;a<3;++a)world[a]=sample(q,[&](const C& c){return double(tree.getValue(c)[a]);});}
    return frame.vector(world);
  }
  double pressure(const V& index)const{const V q=mapping->at(index);return sample(q,[&](const C& c){return scalarAt(*grid,c);});}
  void preflight(const openvdb::CoordBBox& box,const Affine& frame)const{if(!grid)return;for(int c=0;c<8;++c){V q;for(int a=0;a<3;++a)q[a]=(c&(1<<a))?double(box.max()[a])+2:double(box.min()[a])-2;(void)frame.world(q);if(mapping)(void)mapping->at(q);}}
};
void options(const BoundaryProjectionOptions& o,const ProcessingOptions& p,bool replay){
  const auto& s=o.projection;
  if(!std::isfinite(s.timeStep)||s.timeStep<1e-6||s.timeStep>3600||!std::isfinite(s.tolerance)||s.tolerance<1e-12||s.tolerance>1e-2||!s.maxIterations||s.maxIterations>10000||!s.maxFluidCells||s.maxFluidCells>1'000'000||!o.maxWorkItems||o.maxWorkItems>10'000'000'000ULL||!o.maxGrids||o.maxGrids>4096||!o.maxMetadataBytes||!p.maxInputBytes||!p.maxVoxelCount)
    throw std::invalid_argument("boundary projection dt/tolerance/iteration/cell/work/budget controls are invalid");
  for(double a:s.acceleration)if(!std::isfinite(a)||std::abs(a)>1e12)throw std::invalid_argument("boundary projection WORLD acceleration exceeds finite1e12 cap");
  for(double v:o.wallVelocityWorld)if(!std::isfinite(v)||std::abs(v)>1e12)throw std::invalid_argument("boundary projection WORLD wall velocity exceeds finite1e12 cap");
  if(!std::isfinite(o.ambientPressure)||!std::isfinite(o.minimumInterfaceFraction)||o.minimumInterfaceFraction<1e-6||o.minimumInterfaceFraction>1||o.missing<MissingFluidBoundary::ClosedWall||o.missing>MissingFluidBoundary::FreeSurfacePressure||o.airPressure<AirPressureLocation::NeighborCenter||o.airPressure>AirPressureLocation::LevelSetInterface)
    throw std::invalid_argument("boundary projection pressure/interface policy is invalid");
  if(o.writeBoundaryState&&!s.writeDomain)throw std::invalid_argument("canonical boundary state requires the fluid-domain output");
  std::set<std::string> names;for(const auto* n:{&s.velocityGridName,&s.pressureGridName,&s.domainGridName,&o.faceKindGridName,&o.faceFractionGridName,&o.facePressureGridName,&o.wallVelocityGridName})if(n->empty()||n->size()>256||!names.insert(*n).second)throw std::invalid_argument("boundary projection seven output names must be distinct1..256byte names");
  if(replay&&(o.missing!=MissingFluidBoundary::ClosedWall||o.airPressure!=AirPressureLocation::NeighborCenter||o.minimumInterfaceFraction!=.01||o.wallVelocityWorld!=std::array<double,3>{0,0,0}||o.ambientPressure!=0))throw std::invalid_argument("canonical boundary replay excludes geometric/constant boundary controls");
}
void outputPath(const std::filesystem::path& path){if(path.empty()||std::filesystem::exists(path)||!std::filesystem::is_directory(path.has_parent_path()?path.parent_path():std::filesystem::path(".")))throw std::invalid_argument("boundary projection output must be new in an existing directory");}
template<class F>void maskCells(const openvdb::GridBase::Ptr& grid,F&& fn){if(!grid)return;dispatch(*grid,[&](const auto& g){using T=typename std::decay_t<decltype(g)>::ValueType;if constexpr(std::is_arithmetic_v<T>)for(auto v=g.cbeginValueOn();v;++v)if(*v>0){const auto b=v.getBoundingBox();for(int a=0;a<3;++a)if(b.min()[a]<INT32_MIN+8192||b.max()[a]>INT32_MAX-8192)throw std::invalid_argument("boundary projection mask coordinates exceed native margins");for(auto c=b.begin();c;++c)fn(*c);}});}
struct Face {C coordinate;int axis=0,left=-1,right=-1,kind=0;double theta=0,pD=0,wall=0,predictor=0,value=0;};
struct Component {std::vector<int32_t> cells;bool dirichlet=false;double reference=0,scale=0;};
struct Catalog {
  std::set<C> leaves,lower,upper;
  void add(const C& c){leaves.insert(c&~7);lower.insert(c&~127);upper.insert(c&~4095);}
  template<class Grid>long double bytes()const{using Tree=typename Grid::TreeType;using Upper=typename Tree::RootNodeType::ChildNodeType;using Lower=typename Upper::ChildNodeType;using Leaf=typename Lower::ChildNodeType;
    return leaves.size()*(sizeof(Leaf)+512.0L*sizeof(typename Grid::ValueType)+1024)+lower.size()*(sizeof(Lower)+1024.0L)+upper.size()*(sizeof(Upper)+1024.0L)+upper.size()*128.0L+4096;}
};
struct Plan {
  std::vector<C> cells;std::vector<std::array<int32_t,6>> incident;std::vector<Face> faces;std::vector<Component> components;
  std::vector<double> divergence,pressure;Catalog cellNodes,faceNodes;std::set<C> faceCoordinates;
  BoundaryProjectionResult result;long double reserve=0,work=0;
  int index(const C& c)const{auto i=std::lower_bound(cells.begin(),cells.end(),c);return i!=cells.end()&&*i==c?int(i-cells.begin()):-1;}
};
void classifyFaces(Plan& q){
  q.incident.resize(q.cells.size());for(auto& x:q.incident)x.fill(-1);
  for(size_t i=0;i<q.cells.size();++i){const auto c=q.cells[i];q.cellNodes.add(c);
    for(int a=0;a<3;++a){C e(0);e[a]=1;const int left=q.index(c-e),right=q.index(c+e);
      Face f;f.coordinate=c;f.axis=a;f.left=left;f.right=int(i);int n=int(q.faces.size());q.faces.push_back(f);q.incident[i][2*a]=n;if(left>=0)q.incident[left][2*a+1]=n;
      if(right<0){f.coordinate=c+e;f.left=int(i);f.right=-1;n=int(q.faces.size());q.faces.push_back(f);q.incident[i][2*a+1]=n;}
    }
  }
  for(const auto& f:q.faces){q.faceNodes.add(f.coordinate);q.faceCoordinates.insert(f.coordinate);}
}
void components(Plan& q){std::vector<bool> seen(q.cells.size(),false);std::vector<int32_t> queue;queue.reserve(q.cells.size());
  for(size_t start=0;start<q.cells.size();++start)if(!seen[start]){Component c;queue.push_back(int32_t(start));seen[start]=true;
    while(!queue.empty()){int i=queue.back();queue.pop_back();c.cells.push_back(i);for(int n:q.incident[i]){const auto& f=q.faces[n];const int j=f.left==i?f.right:f.left;if(j>=0&&!seen[j]){seen[j]=true;queue.push_back(j);}}}
    std::sort(c.cells.begin(),c.cells.end());q.components.push_back(std::move(c));}
}
void preflightPlan(Plan& q,const Loader& reader,const BoundaryProjectionOptions& o,const ProcessingOptions& p,long double outputMetadata){
  if(q.cells.empty())throw std::invalid_argument("boundary projection fluid domain is empty");
  const long double slots=reader.slots+512.0L*(2*q.cellNodes.leaves.size()+(o.writeBoundaryState?5:1)*q.faceNodes.leaves.size());
  if(slots>p.maxVoxelCount)throw std::invalid_argument("boundary projection joint source/seven-output leaf slots exceed voxel cap");
  const long double trees=q.cellNodes.bytes<Mask>()+q.cellNodes.bytes<Scalar>()+q.faceNodes.bytes<Mac>()+
    (o.writeBoundaryState?q.faceNodes.bytes<openvdb::Vec3IGrid>()+3*q.faceNodes.bytes<Mac>():0);
  q.work=reader.work+q.cells.size()*8192.0L+q.faces.size()*4096.0L+q.cells.size()*7.0L*(64+32.0L*o.projection.maxIterations)+slots*32+outputMetadata*8+Fixed;
  if(!std::isfinite(q.work)||q.work>o.maxWorkItems)throw std::invalid_argument("boundary projection total traversal/PCG attempts/seven-output work exceeds maxWorkItems before solver allocation");
  if(reader.meta+outputMetadata>o.maxMetadataBytes)throw std::invalid_argument("boundary projection joint source/output metadata exceeds cap");
  q.reserve=reader.resident()+16*trees+q.cells.size()*4096.0L+q.faces.size()*1024.0L+16*outputMetadata+Fixed;
  budget(q.reserve,p,"boundary projection joint seven-grid/matrix/preconditioner/Krylov/writer workspace exceeds byte cap before allocation");
  q.result.workUpperBound=uint64_t(std::ceil(q.work));
}
struct Loaded {
  openvdb::GridBase::Ptr velocity,fluid,solid,open,air,phi,wall,pressure;
  openvdb::GridBase::Ptr kinds,fractions,prescribed;
  openvdb::MetaMap::Ptr fileMetadata;bool replay=false;
};
Plan geometry(Loaded& in,Loader& reader,const BoundaryProjectionOptions& o,const ProcessingOptions& p,const Affine& frame){
  const auto& transform=in.fluid->transform();for(const auto& g:{in.fluid,in.solid,in.open,in.air})mask(g,transform);
  bool surface=o.missing==MissingFluidBoundary::FreeSurfacePressure||(in.air&&o.airPressure==AirPressureLocation::LevelSetInterface);
  if(surface!=bool(in.phi)||(!surface&&o.minimumInterfaceFraction!=.01)||(!in.air&&o.airPressure!=AirPressureLocation::NeighborCenter))throw std::invalid_argument("boundary projection phi/fraction/air-interface controls are missing or irrelevant");
  if(in.phi){nativeTags(*in.phi);if(!same(in.phi->transform(),transform)||!in.phi->isInWorldSpace()||in.phi->getVectorType()!=openvdb::VEC_INVARIANT||in.phi->getGridClass()==openvdb::GRID_STAGGERED)throw std::invalid_argument("boundary projection phi requires the exact fluid map and WORLD invariant scalar semantics");}
  if(in.wall&&o.wallVelocityWorld!=std::array<double,3>{0,0,0})throw std::invalid_argument("boundary projection wall field excludes a nonzero constant wall velocity");
  if(in.pressure&&o.ambientPressure!=0)throw std::invalid_argument("boundary projection pressure field excludes nonzero ambient pressure");
  if(in.fluid->activeVoxelCount()>std::min(o.projection.maxFluidCells,p.maxVoxelCount))throw std::invalid_argument("boundary projection active fluid mask exceeds maxFluidCells before expansion");
  long double visits=0;for(const auto& g:{in.fluid,in.solid,in.open,in.air})if(g)visits+=g->activeVoxelCount();
  if(reader.work+visits*128>o.maxWorkItems)throw std::invalid_argument("boundary projection region overlap traversal exceeds work cap before expansion");reader.work+=visits*128;
  std::array<openvdb::GridBase::Ptr,4> regions{in.fluid,in.solid,in.open,in.air};
  for(size_t a=0;a<regions.size();++a)maskCells(regions[a],[&](const C& c){for(size_t b=a+1;b<regions.size();++b)if(selected(regions[b],c))throw std::invalid_argument("boundary projection explicit fluid/solid/open/air masks overlap");});
  if(in.fluid->activeVoxelCount()>std::min(o.projection.maxFluidCells,p.maxVoxelCount))throw std::invalid_argument("boundary projection active fluid mask exceeds maxFluidCells before expansion");
  Plan q;budget(reader.resident()+in.fluid->activeVoxelCount()*2048.0L+Fixed,p,"boundary projection cell/face catalogs exceed byte cap before expansion");
  q.cells.reserve(size_t(in.fluid->activeVoxelCount()));maskCells(in.fluid,[&](const C& c){q.cells.push_back(c);});std::sort(q.cells.begin(),q.cells.end());
  if(q.cells.empty())throw std::invalid_argument("boundary projection fluid domain is empty");
  q.faces.reserve(6*q.cells.size());classifyFaces(q);components(q);
  const long double outMeta=metadata(*in.velocity)+2*metadata(*in.fluid)+(o.writeBoundaryState?metadata(*in.fluid)+metadata(in.pressure?*in.pressure:*in.fluid)+metadata(in.phi?*in.phi:*in.fluid)+metadata(in.wall?*in.wall:*in.fluid):0)+metadata(*in.fileMetadata)+7*4096.0L;
  preflightPlan(q,reader,o,p,outMeta);
  openvdb::CoordBBox box;for(const auto& c:q.cells)box.expand(c);
  Field velocity(in.velocity,transform,frame,true),wall(in.wall,transform,frame,true),pressure(in.pressure,transform,frame,false);
  velocity.preflight(box,frame);wall.preflight(box,frame);pressure.preflight(box,frame);
  const V force=frame.vector(V(o.projection.acceleration[0],o.projection.acceleration[1],o.projection.acceleration[2]),o.projection.timeStep);
  const V constantWall=frame.vector(V(o.wallVelocityWorld[0],o.wallVelocityWorld[1],o.wallVelocityWorld[2]));
  for(auto& f:q.faces){
    if(f.left>=0&&f.right>=0){f.kind=1;f.theta=1;++q.result.fluidFluidFaces;}
    else{const bool fluidLeft=f.left>=0;C outside=f.coordinate;if(!fluidLeft)--outside[f.axis];
      if(selected(in.solid,outside))f.kind=2;
      else if(selected(in.open,outside))f.kind=3;
      else if(selected(in.air,outside))f.kind=o.airPressure==AirPressureLocation::NeighborCenter?4:5;
      else f.kind=2+int(o.missing);
      if(f.kind==2){f.wall=wall.grid?wall.vector(f.coordinate,f.axis,frame)[f.axis]:constantWall[f.axis];++q.result.wallFaces;}
      else{f.theta=f.kind==3?.5:1;
        if(f.kind==5){const C fluid=q.cells[fluidLeft?f.left:f.right];const double phiF=scalarAt(*in.phi,fluid),phiA=scalarAt(*in.phi,outside);
          if(!(phiF<0)||phiA<0)throw std::invalid_argument("free-surface boundary requires phiFluid<0 and phiAir>=0");
          const double magnitude=std::max(-phiF,phiA),fraction=(-phiF/magnitude)/((-phiF/magnitude)+(phiA/magnitude));
          f.theta=std::max(fraction,o.minimumInterfaceFraction);if(fraction<o.minimumInterfaceFraction){++q.result.clampedInterfaceFaces;q.result.maximumFractionClamp=std::max(q.result.maximumFractionClamp,o.minimumInterfaceFraction-fraction);}
          ++q.result.freeSurfacePressureFaces;
        }else if(f.kind==3)++q.result.openFacePressureFaces;else ++q.result.airCellPressureFaces;
        V location=q.cells[fluidLeft?f.left:f.right].asVec3d();location[f.axis]+=(fluidLeft?1:-1)*f.theta;
        f.pD=pressure.grid?pressure.pressure(location):o.ambientPressure;q.result.minimumEffectiveFraction=std::min(q.result.minimumEffectiveFraction,f.theta);
      }
    }
    if(f.kind==2)f.predictor=f.wall;
    else{const double value=velocity.vector(f.coordinate,f.axis,frame)[f.axis];Sum s;s.add(value);s.add(force[f.axis]);f.predictor=s.get();if(force[f.axis]!=0&&f.predictor==value)throw std::invalid_argument("boundary projection body-force increment is unresolved in stored predictor");}
    if(!std::isfinite(f.predictor)||std::abs(f.predictor)>1e24)throw std::invalid_argument("boundary projection predictor/wall MAC exceeds finite1e24 cap");f.value=f.predictor;
  }
  return q;
}
constexpr const char* StateId="container:boundary_projection_state_id";
constexpr const char* ClampCount="container:boundary_clamped_faces";
constexpr const char* ClampMaximum="container:boundary_maximum_clamp";
std::string schema(const openvdb::GridBase& g){
  const auto s=g.getMetadata<openvdb::StringMetadata>(Schema),layout=g.getMetadata<openvdb::StringMetadata>(Layout),id=g.getMetadata<openvdb::StringMetadata>(StateId);
  if(!s||s->value()!=Version||!layout||layout->value()!="negative_faces_xyz"||!id||id->value().empty()||id->value().size()>256)throw std::invalid_argument("canonical boundary replay is missing valid schema/layout/state identifier metadata");return id->value();
}
void packed(const openvdb::GridBase& g,const openvdb::math::Transform& transform){nativeTags(g);if(!same(g.transform(),transform)||g.getGridClass()!=openvdb::GRID_UNKNOWN||!g.isInWorldSpace()||g.getVectorType()!=openvdb::VEC_INVARIANT)throw std::invalid_argument("canonical boundary face channels require exact-map UNKNOWN WORLD invariant raw tuples");}
Plan canonical(Loaded& in,Loader& reader,const BoundaryProjectionOptions& o,const ProcessingOptions& p,const Affine& frame){
  if(!in.fluid->isType<Mask>()||!in.velocity->isType<Mac>()||!in.wall->isType<Mac>())throw std::invalid_argument("canonical boundary state requires Bool fluid and Double MAC velocity/wall grids");
  const auto& transform=in.fluid->transform();mask(in.fluid,transform);const auto id=schema(*in.fluid);
  packed(*in.fluid,transform);
  const auto& fluid=static_cast<const Mask&>(*in.fluid);
  for(auto v=fluid.cbeginValueOn();v;++v)if(!*v)throw std::invalid_argument("canonical fluid must have only active true records");
  for(auto v=fluid.cbeginValueOff();v;++v)if(*v)throw std::invalid_argument("canonical fluid inactive records must be false");
  for(const auto& g:{in.velocity,in.kinds,in.fractions,in.prescribed,in.wall})if(schema(*g)!=id)throw std::invalid_argument("canonical boundary snapshot identifiers differ");
  for(const auto& g:{in.kinds,in.fractions,in.prescribed})packed(*g,transform);
  if(in.fluid->activeVoxelCount()>o.projection.maxFluidCells)throw std::invalid_argument("canonical fluid topology exceeds maxFluidCells");
  budget(reader.resident()+in.fluid->activeVoxelCount()*2048.0L+Fixed,p,"canonical boundary catalogs exceed byte cap before expansion");
  Plan q;q.result.replayed=true;q.cells.reserve(size_t(in.fluid->activeVoxelCount()));maskCells(in.fluid,[&](const C& c){q.cells.push_back(c);});std::sort(q.cells.begin(),q.cells.end());
  if(q.cells.empty())throw std::invalid_argument("canonical boundary fluid domain is empty");q.faces.reserve(6*q.cells.size());classifyFaces(q);components(q);
  const long double outMeta=metadata(*in.velocity)+2*metadata(*in.fluid)+(o.writeBoundaryState?metadata(*in.kinds)+metadata(*in.fractions)+metadata(*in.prescribed)+metadata(*in.wall):0)+metadata(*in.fileMetadata)+7*4096.0L;
  preflightPlan(q,reader,o,p,outMeta);
  const auto& kinds=static_cast<const openvdb::Vec3IGrid&>(*in.kinds);const auto& fractions=static_cast<const Mac&>(*in.fractions);const auto& prescribed=static_cast<const Mac&>(*in.prescribed);const auto& walls=static_cast<const Mac&>(*in.wall);
  for(const auto& g:{in.velocity,in.kinds,in.fractions,in.prescribed,in.wall}){
    if(g->activeVoxelCount()!=q.faceCoordinates.size())throw std::invalid_argument("canonical boundary face activity differs from incident-face coordinates");
    for(const auto& c:q.faceCoordinates)if(!dispatch(*g,[&](const auto& value){return value.tree().isValueOn(c);}))throw std::invalid_argument("canonical boundary expected face coordinate is inactive");
    dispatch(*g,[&](const auto& value){using T=typename std::decay_t<decltype(value)>::ValueType;if(value.background()!=openvdb::zeroVal<T>())throw std::invalid_argument("canonical boundary face backgrounds must be zero");for(auto v=value.cbeginValueOff();v;++v)if(*v!=openvdb::zeroVal<T>())throw std::invalid_argument("canonical boundary inactive face data must be zero");});
  }
  const auto& nativeVelocity=static_cast<const Mac&>(*in.velocity);
  for(const auto& c:q.faceCoordinates)for(int a=0;a<3;++a){C previous=c;--previous[a];const bool incident=q.index(c)>=0||q.index(previous)>=0;
    if(!incident&&(kinds.tree().getValue(c)[a]!=0||nativeVelocity.tree().getValue(c)[a]!=0||fractions.tree().getValue(c)[a]!=0||prescribed.tree().getValue(c)[a]!=0||walls.tree().getValue(c)[a]!=0))throw std::invalid_argument("canonical absent face components must have kind zero and zero payload");}
  Field velocity(in.velocity,transform,frame,true),wall(in.wall,transform,frame,true);openvdb::CoordBBox box;for(const auto& c:q.cells)box.expand(c);velocity.preflight(box,frame);
  if(!velocity.mac||!wall.mac)throw std::invalid_argument("canonical velocity and wall fields require the native local MAC frame");
  const V force=frame.vector(V(o.projection.acceleration[0],o.projection.acceleration[1],o.projection.acceleration[2]),o.projection.timeStep);
  for(auto& f:q.faces){f.kind=kinds.tree().getValue(f.coordinate)[f.axis];f.theta=fractions.tree().getValue(f.coordinate)[f.axis];f.pD=prescribed.tree().getValue(f.coordinate)[f.axis];f.wall=walls.tree().getValue(f.coordinate)[f.axis];
    if(f.left>=0&&f.right>=0){if(f.kind!=1||f.theta!=1||f.pD!=0||f.wall!=0)throw std::invalid_argument("canonical fluid-fluid face record is inconsistent");++q.result.fluidFluidFaces;}
    else if(f.kind<2||f.kind>5)throw std::invalid_argument("canonical boundary face kind is inconsistent with fluid adjacency");
    else if(f.kind==2){if(f.theta!=0||f.pD!=0)throw std::invalid_argument("canonical wall face has pressure/fraction data");++q.result.wallFaces;}
    else{if(f.wall!=0||f.theta<1e-6||f.theta>1||(f.kind==3&&f.theta!=.5)||(f.kind==4&&f.theta!=1))throw std::invalid_argument("canonical pressure face fraction/wall payload is invalid");q.result.minimumEffectiveFraction=std::min(q.result.minimumEffectiveFraction,f.theta);if(f.kind==3)++q.result.openFacePressureFaces;else if(f.kind==4)++q.result.airCellPressureFaces;else ++q.result.freeSurfacePressureFaces;}
    if(f.kind==2)f.predictor=f.wall;else{const double value=velocity.vector(f.coordinate,f.axis,frame)[f.axis];Sum s;s.add(value);s.add(force[f.axis]);f.predictor=s.get();if(force[f.axis]!=0&&f.predictor==value)throw std::invalid_argument("canonical boundary body-force increment is unresolved");}
    if(!std::isfinite(f.predictor)||std::abs(f.predictor)>1e24)throw std::invalid_argument("canonical boundary predictor/wall exceeds MAC cap");f.value=f.predictor;
  }
  const auto clamped=in.fractions->getMetadata<openvdb::Int64Metadata>(ClampCount);
  const auto maximum=in.fractions->getMetadata<openvdb::DoubleMetadata>(ClampMaximum);
  if(!clamped||clamped->value()<0||uint64_t(clamped->value())>q.result.freeSurfacePressureFaces||!maximum||!std::isfinite(maximum->value())||maximum->value()<0||maximum->value()>1||((clamped->value()==0)!=(maximum->value()==0)))throw std::invalid_argument("canonical boundary interface clamp diagnostics are invalid");
  q.result.clampedInterfaceFaces=uint64_t(clamped->value());q.result.maximumFractionClamp=maximum->value();
  return q;
}
double divergence(const Plan& q,size_t cell,bool stored){Sum s;for(int a=0;a<3;++a){const auto& minus=q.faces[q.incident[cell][2*a]];const auto& plus=q.faces[q.incident[cell][2*a+1]];s.add(stored?plus.value:plus.predictor);s.add(-(stored?minus.value:minus.predictor));}return s.get();}
double roundoff(const Plan& q,size_t cell){Sum s;for(int n:q.incident[cell])s.add(gap(q.faces[n].value));return scale(s.get(),{8});}
void project(Plan& q,const Affine& frame,const BoundaryProjectionOptions& options){
  const auto& p=options.projection;const double k=scale(p.timeStep,{1},{frame.h,frame.h});
  q.divergence.resize(q.cells.size());q.pressure.assign(q.cells.size(),0);std::vector<int32_t> local(q.cells.size(),-1);
  auto& report=q.result.projection;report.fluidCellCount=q.cells.size();report.faceCount=q.result.fluidFluidFaces;report.componentCount=q.components.size();
  for(size_t i=0;i<q.cells.size();++i){q.divergence[i]=divergence(q,i,false);report.maxDivergenceBefore=std::max(report.maxDivergenceBefore,std::abs(q.divergence[i]));}
  for(auto& component:q.components){Sum flux;double magnitude=0;
    for(size_t row=0;row<component.cells.size();++row){const int i=component.cells[row];local[i]=int32_t(row);component.scale=std::max(component.scale,std::abs(q.divergence[i]));
      for(int n:q.incident[i]){const auto& f=q.faces[n];if(f.kind>=3&&!component.dirichlet){component.dirichlet=true;component.reference=f.pD;}if(f.kind==2){double signedFlux=f.left==i?f.wall:-f.wall;flux.add(signedFlux);magnitude+=std::abs(f.wall);}}
    }
    if(component.dirichlet){++q.result.dirichletComponents;for(int i:component.cells)for(int n:q.incident[i]){const auto& f=q.faces[n];if(f.kind>=3)component.scale=std::max(component.scale,std::abs(scale(difference(f.pD,component.reference),{k},{f.theta})));}}
    else{++q.result.neumannComponents;double net=std::abs(flux.get());q.result.maxClosedNormalFlux=std::max(q.result.maxClosedNormalFlux,net);if(net>256*std::numeric_limits<double>::epsilon()*magnitude)throw std::runtime_error("closed moving-wall normal flux is incompatible; no divergence mean repair or output");}
    const auto count=component.cells.size();Vector solution(openvdb::math::pcg::SizeType(count),0);
    if(component.scale!=0){Matrix matrix{openvdb::math::pcg::SizeType(count)};Vector rhs(solution.size(),0);
      for(size_t row=0;row<count;++row){const int i=component.cells[row];auto edit=matrix.getRowEditor(openvdb::math::pcg::SizeType(row));
        if(!component.dirichlet&&row==0){edit.setValue(0,1);continue;}Sum right;right.add(-scale(q.divergence[i],{1},{component.scale}));double diagonal=0;
        for(int n:q.incident[i]){const auto& f=q.faces[n];if(f.kind==1){++diagonal;int neighbor=f.left==i?f.right:f.left;int column=local[neighbor];if(component.dirichlet||column!=0)edit.setValue(column,-1);}
          else if(f.kind>=3){diagonal+=1/f.theta;right.add(scale(difference(f.pD,component.reference),{k},{f.theta,component.scale}));}}
        if(!(diagonal>0))throw std::runtime_error("boundary projection nonzero isolated closed RHS is incompatible");rhs[row]=right.get();edit.setValue(openvdb::math::pcg::SizeType(row),diagonal);
      }
      auto termination=openvdb::math::pcg::terminationDefaults<double>();termination.relativeError=p.tolerance*.025/std::sqrt(double(count));termination.absoluteError=0;
      std::unique_ptr<openvdb::math::pcg::Preconditioner<double>> preconditioner(new openvdb::math::pcg::IncompleteCholeskyPreconditioner<Matrix>(matrix));if(!preconditioner->isValid())preconditioner=std::make_unique<openvdb::math::pcg::JacobiPreconditioner<Matrix>>(matrix);
      openvdb::util::NullInterrupter interrupter;auto state=termination;uint32_t iterations=0;
      do{termination.iterations=int(p.maxIterations-iterations);state=openvdb::math::pcg::solve(matrix,rhs,solution,*preconditioner,interrupter,termination);iterations+=uint32_t(std::max(0,state.iterations));if(state.success||!solution.isFinite()||state.iterations<=0||!std::isfinite(state.relativeError)||iterations>=p.maxIterations)break;preconditioner=std::make_unique<openvdb::math::pcg::JacobiPreconditioner<Matrix>>(matrix);}while(iterations<p.maxIterations);
      q.result.totalIterations+=iterations;report.iterations=std::max(report.iterations,iterations);if(!state.success||!solution.isFinite())throw std::runtime_error("boundary projection native PCG did not converge within original iteration/work caps");
    }
    for(size_t row=0;row<count;++row){const int i=component.cells[row];Sum pressure;pressure.add(component.reference);pressure.add(scale(solution[row],{component.scale},{k}));q.pressure[i]=pressure.get();}
    if(component.scale!=0)for(int i:component.cells){Sum residual;residual.add(scale(q.divergence[i],{1},{component.scale}));for(int n:q.incident[i]){const auto& f=q.faces[n];if(f.kind==1){const int neighbor=f.left==i?f.right:f.left;residual.add(scale(difference(q.pressure[i],q.pressure[neighbor]),{k},{component.scale}));}
        else if(f.kind>=3)residual.add(scale(difference(q.pressure[i],f.pD),{k},{f.theta,component.scale}));}
      double error=std::abs(residual.get());report.residual=std::max(report.residual,error);if(error>p.tolerance*.5)throw std::runtime_error("boundary projection full stored-pressure residual including gauge exceeds tolerance");}
    for(int i:component.cells)for(int n:q.incident[i]){auto& f=q.faces[n];if(f.kind==2)continue;if(f.kind==1&&f.right!=i)continue;
      const double left=f.left>=0?q.pressure[f.left]:f.pD,right=f.right>=0?q.pressure[f.right]:f.pD;
      const double correction=scale(difference(right,left),{k},{f.theta});Sum value;value.add(f.predictor);value.add(-correction);f.value=value.get();if(std::abs(f.value)>1e24)throw std::invalid_argument("boundary projected MAC exceeds1e24 cap");}
    for(int i:component.cells){const double after=std::abs(divergence(q,i,true)),allowance=roundoff(q,i);report.maxDivergenceAfter=std::max(report.maxDivergenceAfter,after);report.divergenceRoundoffBound=std::max(report.divergenceRoundoffBound,allowance);
      // A diagnostic tolerance product may underflow to zero; exact zero
      // residuals still satisfy it. Field/correction scaling never uses it.
      const double target=component.scale*p.tolerance;if(after>target){report.roundoffLimited=true;++q.result.roundoffLimitedCells;}if(after>target+allowance)throw std::runtime_error("boundary projection full stored-face divergence exceeds relative/ULP tolerance");}
  }
  for(const auto& f:q.faces)if(f.kind==2){q.result.maxWallNormalError=std::max(q.result.maxWallNormalError,std::abs(difference(f.value,f.wall)));if(f.value!=f.wall)throw std::runtime_error("boundary projection changed a prescribed wall normal");}
  report.converged=true;
}
template<class Grid>struct ExactWrite:Grid {
  explicit ExactWrite(const Grid& source):Grid(openvdb::ConstPtrCast<typename Grid::TreeType>(source.constTreePtr())){this->insertMeta(source);this->setTransform(detail::CopyTransformExact(source.transform()));}
  openvdb::GridBase::ConstPtr copyGrid()const override{return this->copyReplacingMetadataAndTransform(*this,detail::CopyTransformExact(this->transform()));}
  openvdb::GridBase::Ptr copyGrid()override{return openvdb::ConstPtrCast<openvdb::GridBase>(static_cast<const ExactWrite&>(*this).copyGrid());}
};
void publish(const std::filesystem::path& source,const std::filesystem::path& output){bool owned=false;std::ofstream target;try{std::ifstream input(source,std::ios::binary);if(!input)throw std::runtime_error("cannot read completed boundary projection");target.open(output,std::ios::binary|std::ios::out|std::ios::noreplace);owned=target.is_open();if(!owned)throw std::runtime_error("cannot exclusively create boundary projection output");input.exceptions(std::ios::badbit);target.exceptions(std::ios::badbit|std::ios::failbit);std::array<char,65536> buffer{};while(input.read(buffer.data(),std::streamsize(buffer.size()))||input.gcount())target.write(buffer.data(),input.gcount());if(!input.eof())throw std::runtime_error("boundary projection completed output read failed");target.close();}catch(...){target.exceptions(std::ios::goodbit);if(target.is_open())target.close();if(owned){std::error_code e;std::filesystem::remove(output,e);}throw;}}
void structure(openvdb::GridBase& g,const openvdb::GridBase& source,const openvdb::math::Transform& transform,const std::string& name,bool mac,const std::string& id){
  g.insertMeta(source);g.setTransform(detail::CopyTransformExact(transform));
  for(const auto* key:{openvdb::GridBase::META_GRID_NAME,openvdb::GridBase::META_GRID_CLASS,openvdb::GridBase::META_IS_LOCAL_SPACE,openvdb::GridBase::META_VECTOR_TYPE,openvdb::GridBase::META_SAVE_HALF_FLOAT,openvdb::GridBase::META_FILE_BBOX_MIN,openvdb::GridBase::META_FILE_BBOX_MAX,openvdb::GridBase::META_FILE_MEM_BYTES,openvdb::GridBase::META_FILE_VOXEL_COUNT,openvdb::GridBase::META_FILE_COMPRESSION,openvdb::GridBase::META_FILE_DELAYED_LOAD})g.removeMeta(key);
  for(const char* key:{Schema,Layout,StateId,ClampCount,ClampMaximum,"container:mac_layout","container:velocity_units","container:pressure_units","container:pressure_gauge","container:projection_dt","container:potential_flow_units","container:potential_flow_boundary","container:surface_field_units","container:surface_field_validity","container:surface_field_location"})g.removeMeta(key);
  g.setName(name);g.setGridClass(mac?openvdb::GRID_STAGGERED:openvdb::GRID_UNKNOWN);g.setVectorType(mac?openvdb::VEC_CONTRAVARIANT_RELATIVE:openvdb::VEC_INVARIANT);g.setIsInWorldSpace(!mac);g.setSaveFloatAsHalf(false);
  if(mac){g.insertMeta("container:mac_layout",openvdb::StringMetadata("negative_faces"));g.insertMeta("container:velocity_units",openvdb::StringMetadata("voxels_per_second"));}
  if(!id.empty()){g.insertMeta(Schema,openvdb::StringMetadata(Version));g.insertMeta(Layout,openvdb::StringMetadata("negative_faces_xyz"));g.insertMeta(StateId,openvdb::StringMetadata(id));}
}
template<class Grid,class T>void setComponent(Grid& grid,const C& c,int a,T x){auto v=grid.tree().getValue(c);v[a]=x;grid.tree().touchLeaf(c)->setValueOn(c,v);}
BoundaryProjectionResult finish(Loaded& in,Plan& q,Loader& reader,const Affine& frame,const std::filesystem::path& output,const BoundaryProjectionOptions& o,const ProcessingOptions& p){
  project(q,frame,o);
  auto velocity=Mac::create(V(0));auto pressure=Scalar::create(0);auto fluid=Mask::create(false);
  openvdb::Vec3IGrid::Ptr kinds;Mac::Ptr fractions,prescribed,walls;
  if(o.writeBoundaryState){kinds=openvdb::Vec3IGrid::create(openvdb::Vec3i(0));fractions=Mac::create(V(0));prescribed=Mac::create(V(0));walls=Mac::create(V(0));}
  for(size_t i=0;i<q.cells.size();++i){pressure->tree().touchLeaf(q.cells[i])->setValueOn(q.cells[i],q.pressure[i]);fluid->tree().touchLeaf(q.cells[i])->setValueOn(q.cells[i],true);}
  for(const auto& f:q.faces){setComponent(*velocity,f.coordinate,f.axis,f.value);if(o.writeBoundaryState){setComponent(*kinds,f.coordinate,f.axis,f.kind);setComponent(*fractions,f.coordinate,f.axis,f.theta);setComponent(*prescribed,f.coordinate,f.axis,f.pD);setComponent(*walls,f.coordinate,f.axis,f.wall);}}
  const auto& s=o.projection;const auto& transform=in.fluid->transform();const std::string id=o.writeBoundaryState?reader.temporary().parent_path().filename().string():std::string{};
  structure(*velocity,*in.velocity,transform,s.velocityGridName,true,id);structure(*pressure,*in.fluid,transform,s.pressureGridName,false,id);structure(*fluid,*in.fluid,transform,s.domainGridName,false,id);
  velocity->insertMeta("container:projection_dt",openvdb::DoubleMetadata(s.timeStep));pressure->insertMeta("container:pressure_units",openvdb::StringMetadata("world_distance_squared_per_second_squared"));pressure->insertMeta("container:pressure_gauge",openvdb::StringMetadata("closed_lexicographic_zero;dirichlet_absolute"));
  openvdb::GridPtrVec outputs{velocity,pressure};if(s.writeDomain)outputs.push_back(fluid);
  if(o.writeBoundaryState){structure(*kinds,in.kinds?*in.kinds:*in.fluid,transform,o.faceKindGridName,false,id);structure(*fractions,in.fractions?*in.fractions:in.phi?*in.phi:*in.fluid,transform,o.faceFractionGridName,false,id);structure(*prescribed,in.prescribed?*in.prescribed:in.pressure?*in.pressure:*in.fluid,transform,o.facePressureGridName,false,id);structure(*walls,in.wall?*in.wall:*in.fluid,transform,o.wallVelocityGridName,true,id);
    fractions->insertMeta(ClampCount,openvdb::Int64Metadata(int64_t(q.result.clampedInterfaceFaces)));fractions->insertMeta(ClampMaximum,openvdb::DoubleMetadata(q.result.maximumFractionClamp));outputs.insert(outputs.end(),{kinds,fractions,prescribed,walls});}
  long double retained=0,meta=metadata(*in.fileMetadata),slots=reader.slots;
  for(const auto& g:outputs){retained+=g->memUsage();meta+=metadata(*g);slots+=512.0L*g->baseTree().leafCount();}
  if(slots>p.maxVoxelCount||reader.meta+meta>o.maxMetadataBytes)throw std::invalid_argument("boundary projection final shared topology/metadata exceeds caps");budget(q.reserve+8*retained+8*meta+Fixed,p,"boundary projection stored seven-grid writer exceeds shared byte cap");
  q.result.projection.velocityGridName=s.velocityGridName;q.result.projection.pressureGridName=s.pressureGridName;if(s.writeDomain)q.result.projection.domainGridName=s.domainGridName;
  q.result.boundaryStateWritten=o.writeBoundaryState;q.result.diagnostics={
    "One matching face table defines divergence, SPD pressure operator, stored-pressure correction and boundary validation.",
    "Open FACE theta=.5 differs from AIR CENTER theta=1; free-surface fractions use the reported effective clamp with full cell volumes.",
    "Closed moving-wall flux is checked without mean subtraction; pressure is physical unit-density pressure and all gauge rows are independently checked.",
    "Nonuniform prescribed pressure is a fresh pressure-driven impulse; canonical replay is not generally an idempotent projection.",
    "File metadata comes from the fluid source; all original inputs remain unchanged. This is fixed-domain inviscid projection, not a complete free-surface timestep.",
    "Logical full-input/seven-output/solver/work bounds are not a process-RSS sandbox for eager native metadata or legacy decodes."};
  const auto temporary=reader.temporary();struct Cleanup{std::filesystem::path path;~Cleanup(){std::error_code e;std::filesystem::remove(path,e);}}cleanup{temporary};
  openvdb::GridPtrVec views;views.reserve(outputs.size());for(const auto& g:outputs){if(g->isType<Mac>())views.push_back(std::make_shared<ExactWrite<Mac>>(static_cast<const Mac&>(*g)));else if(g->isType<Scalar>())views.push_back(std::make_shared<ExactWrite<Scalar>>(static_cast<const Scalar&>(*g)));else if(g->isType<Mask>())views.push_back(std::make_shared<ExactWrite<Mask>>(static_cast<const Mask&>(*g)));else views.push_back(std::make_shared<ExactWrite<openvdb::Vec3IGrid>>(static_cast<const openvdb::Vec3IGrid&>(*g)));}
  openvdb::io::File writer(temporary.string());writer.setCompression(openvdb::io::COMPRESS_ZIP);writer.write(views,*in.fileMetadata);writer.close();budget(q.reserve+8*retained+8*meta+std::filesystem::file_size(temporary)+Fixed,p,"boundary projection encoded output/writer exceeds shared byte cap");
  static_assert(std::is_nothrow_move_constructible_v<BoundaryProjectionResult>);publish(temporary,output);return std::move(q.result);
}
#endif
} // namespace
BoundaryProjectionResult WriteBoundaryProjectedVelocity(const BoundaryProjectionInputs& input,const std::filesystem::path& output,const BoundaryProjectionOptions& o,const ProcessingOptions& p){
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  options(o,p,false);outputPath(output);static std::once_flag once;std::call_once(once,[]{openvdb::initialize();});tbb::task_arena arena(1);return arena.execute([&]{Loader reader(o,p);Loaded in;
    in.fluid=reader.get(input.fluid,Role::Mask);in.velocity=reader.get(input.velocity,Role::Velocity);in.solid=reader.get(input.solid,Role::Mask,true);in.open=reader.get(input.open,Role::Mask,true);in.air=reader.get(input.air,Role::Mask,true);in.phi=reader.get(input.liquidPhi,Role::Scalar,true);in.wall=reader.get(input.wallVelocity,Role::Velocity,true);in.pressure=reader.get(input.pressure,Role::Scalar,true);in.fileMetadata=reader.fileMetadata(input.fluid);
    const Affine frame(in.fluid->transform(),true);auto q=geometry(in,reader,o,p,frame);return finish(in,q,reader,frame,output,o,p);});
#else
  (void)input;(void)output;(void)o;(void)p;unavailable();
#endif
}
BoundaryProjectionResult WriteReprojectedBoundaryState(const BoundaryProjectionReplay& replay,const std::filesystem::path& output,const BoundaryProjectionOptions& o,const ProcessingOptions& p){
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  options(o,p,true);outputPath(output);static std::once_flag once;std::call_once(once,[]{openvdb::initialize();});tbb::task_arena arena(1);return arena.execute([&]{Loader reader(o,p);Loaded in;in.replay=true;
    in.fluid=reader.get({replay.state,replay.fluidGrid},Role::Mask);in.velocity=reader.get({replay.state,replay.velocityGrid},Role::Velocity);in.kinds=reader.get({replay.state,replay.faceKindGrid},Role::FaceKind);in.fractions=reader.get({replay.state,replay.faceFractionGrid},Role::FaceValues);in.prescribed=reader.get({replay.state,replay.facePressureGrid},Role::FaceValues);in.wall=reader.get({replay.state,replay.wallVelocityGrid},Role::Velocity);in.fileMetadata=reader.fileMetadata({replay.state,replay.fluidGrid});const Affine frame(in.fluid->transform(),true);auto q=canonical(in,reader,o,p,frame);return finish(in,q,reader,frame,output,o,p);});
#else
  (void)replay;(void)output;(void)o;(void)p;unavailable();
#endif
}
} // namespace container::geometry::vdb
