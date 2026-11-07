#include "Container/geometry/OpenVdbFieldAdvection.h"
#include "OpenVdbProvenance.h"
#include <stdexcept>
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
#include "OpenVdbFastSweepingInternal.h"
#include <openvdb/tools/Interpolation.h>
#include <functional>
#endif
namespace container::geometry::vdb {
namespace {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
using namespace fast_detail;
using Vec=openvdb::Vec3d;
struct Sum {
  double value{},correction{};
  void add(double x) {
    const double next=value+x;
    if(!std::isfinite(x) || !std::isfinite(next)) throw std::invalid_argument("field advection affine arithmetic exceeds Double range");
    correction+=std::abs(value)>=std::abs(x)?(value-next)+x:(x-next)+value;value=next;
    if(!std::isfinite(correction)) throw std::invalid_argument("field advection compensated sum overflows");
  }
  void product(double a,double b) {
    const double x=a*b;if(a!=0 && b!=0 && x==0) throw std::invalid_argument("nonzero field advection transform term underflows Double");
    add(x);add(std::fma(a,b,-x));
  }
  double get() const {const double x=value+correction;if(!std::isfinite(x)) throw std::invalid_argument("field advection affine sum overflows");return x;}
};
struct Affine {
  Vec origin;std::array<Vec,3> basis,inverse;std::array<double,3> lengths;
  explicit Affine(const openvdb::GridBase &g) {
    if(!g.transform().isLinear()) throw std::invalid_argument("field advection requires affine sampling maps");
    const auto m=g.transform().baseMap();origin=m->applyMap(Vec(0));std::array<Vec,3> unit;
    for(size_t a=0;a<3;++a) {
      if(!std::isfinite(origin[a])) throw std::invalid_argument("nonfinite field advection affine origin");
      Vec e(0);e[a]=1;basis[a]=m->applyJacobian(e);lengths[a]=std::hypot(basis[a].x(),basis[a].y(),basis[a].z());
      if(!std::isfinite(lengths[a]) || lengths[a]<1e-6 || lengths[a]>1e6) throw std::invalid_argument("field advection axes must span [1e-6,1e6]");
      unit[a]=basis[a]/lengths[a];
    }
    const double d=unit[0].dot(unit[1].cross(unit[2]));
    if(!std::isfinite(d) || std::abs(d)<1e-12) throw std::invalid_argument("field advection affine map is singular/ill-conditioned");
    for(size_t a=0;a<3;++a) {
      inverse[a]=unit[(a+1)%3].cross(unit[(a+2)%3])/d/lengths[a];
      for(size_t c=0;c<3;++c) if(!std::isfinite(inverse[a][c])) throw std::invalid_argument("invalid field advection inverse Jacobian");
    }
  }
  Vec vector(const Vec &v) const {
    Vec out;for(size_t c=0;c<3;++c) {Sum sum;for(size_t a=0;a<3;++a) sum.product(basis[a][c],v[a]);out[c]=sum.get();}return out;
  }
  Vec indexVector(const Vec &v) const {
    Vec out;for(size_t a=0;a<3;++a) {Sum sum;for(size_t c=0;c<3;++c) sum.product(inverse[a][c],v[c]);out[a]=sum.get();}return out;
  }
  Vec relativeIndex(const Affine &source,const Vec &index) const {
    Vec delta;for(size_t c=0;c<3;++c) {Sum sum;sum.add(source.origin[c]-origin[c]);for(size_t a=0;a<3;++a) sum.product(source.basis[a][c],index[a]);delta[c]=sum.get();}
    return indexVector(delta);
  }
};
void validate(const FieldAdvectionOptions &o,const ProcessingOptions &p,const std::filesystem::path &output) {
  if(!std::isfinite(o.timeStep) || std::abs(o.timeStep)>3600 || !o.steps || o.steps>64 || !std::isfinite(o.maxCfl) || !(o.maxCfl>0) || o.maxCfl>16 ||
      o.scheme<FieldAdvectionScheme::Semi || o.scheme>FieldAdvectionScheme::Bfecc || o.limiter<FieldAdvectionLimiter::None || o.limiter>FieldAdvectionLimiter::Revert ||
      o.velocityLayout<FieldVelocityLayout::Auto || o.velocityLayout>FieldVelocityLayout::NativeMacIndex || !o.maxWorkItems || o.maxWorkItems>10'000'000'000ull ||
      !p.maxVoxelCount || !p.maxInputBytes || p.gridName.empty() || p.gridName.size()>256 || p.gridName.find('\0')!=std::string::npos)
    throw std::invalid_argument("invalid field advection time/steps/scheme/limiter/layout/CFL/name/budgets");
  if(o.mask.empty() && !o.maskGrid.empty()) throw std::invalid_argument("field advection mask selector requires a mask file");
  if(output.empty() || std::filesystem::exists(output) || !std::filesystem::is_directory(output.has_parent_path()?output.parent_path():std::filesystem::path(".")))
    throw std::invalid_argument("field advection requires a new output in an existing directory");
}
openvdb::GridBase::Ptr loadVelocity(const std::filesystem::path &path,const std::filesystem::path &snapshot,const std::string &selection,Budget &b) {
  if(path.empty() || !std::filesystem::is_regular_file(path)) throw std::invalid_argument("velocity input must be a regular file");
  const auto encoded=std::filesystem::file_size(path);add(b.encoded,encoded,b.p.maxInputBytes,"combined advection encoded inputs exceed byte cap");b.check();
  copy(path,snapshot,encoded);if(std::filesystem::file_size(snapshot)!=encoded) throw std::runtime_error("velocity input changed size during snapshot");
  detail::RequireSeekableFile(snapshot);if(!openvdb::io::Archive::isDelayedLoadingEnabled()) throw std::invalid_argument("bounded field advection requires delayed loading; unset OPENVDB_DISABLE_DELAYED_LOAD");
  openvdb::io::File file(snapshot.string());file.setCopyMaxBytes(0);file.open(true);b.meta(*file.getMetadata());std::set<std::string> names;
  for(auto it=file.beginName();it!=file.endName();++it) {
    const auto name=it.gridName();add(b.metadata,2*name.size()+128,b.p.maxInputBytes,"advection velocity descriptor cap exceeded");b.check();
    if(names.size()>=4096 || !names.insert(name).second) throw std::invalid_argument("too many/colliding velocity descriptors");
  }
  std::string chosen;bool found=false;
  for(auto it=file.beginName();it!=file.endName();++it) {
    auto meta=file.readGridMetadata(it.gridName());b.meta(*meta);
    if(!GridSelectionMatches(selection,it.gridName(),meta->getName())) continue;
    const bool type=meta->isType<openvdb::Vec3SGrid>() || meta->isType<openvdb::Vec3DGrid>();if(selection.empty() && !type) continue;
    if(!type) throw std::invalid_argument("velocity selector must identify Vec3S/Vec3D");
    if(found) throw std::invalid_argument("ambiguous velocity selector; use descriptor:<exact descriptor>");
    const auto memory=meta->getMetadata<openvdb::Int64Metadata>(openvdb::GridBase::META_FILE_MEM_BYTES),count=meta->getMetadata<openvdb::Int64Metadata>(openvdb::GridBase::META_FILE_VOXEL_COUNT);
    if(!memory || !count || memory->value()<0 || count->value()<0 || uintmax_t(count->value())>b.p.maxVoxelCount) throw std::invalid_argument("advection velocity requires recorded memory/voxel statistics within budget");
    b.check(uintmax_t(memory->value()));chosen=it.gridName();found=true;if(selection.empty()) break;
  }
  if(!found) throw std::invalid_argument("no Vec3S/Vec3D velocity matches selection");
  auto grid=file.readGrid(chosen);const auto memory=fullMemory(*grid);
  add(b.resident,memory,b.p.maxInputBytes,"decoded velocity tree exceeds byte cap");add(b.active,grid->activeVoxelCount(),b.p.maxVoxelCount,"aggregate advection activity exceeds cap");
  add(b.slots,mul(grid->baseTree().leafCount(),512,b.p.maxVoxelCount,"velocity leaf slots exceed cap"),b.p.maxVoxelCount,"aggregate advection leaf slots exceed cap");
  b.items(grid->baseTree().leafCount()*512);b.check();grid->readNonresidentBuffers();if(grid->memUsage()>memory) throw std::runtime_error("decoded advection velocity exceeded preflight");file.close();return grid;
}
bool correctionScheme(FieldAdvectionScheme s) {return s==FieldAdvectionScheme::MacCormack || s==FieldAdvectionScheme::Bfecc;}
template<class Value> constexpr size_t components= openvdb::VecTraits<Value>::IsVec?3:1;
template<class Value> using Element=typename openvdb::VecTraits<Value>::ElementType;
template<class Value> auto component(const Value &v,size_t c) {if constexpr(openvdb::VecTraits<Value>::IsVec) return v[c];else {(void)c;return v;}}
template<class Value> void assign(Value &v,size_t c,Element<Value> x) {if constexpr(openvdb::VecTraits<Value>::IsVec) v[c]=x;else {(void)c;v=x;}}
template<class Grid> void range(const Grid &g,std::array<double,3> &lo,std::array<double,3> &hi,Budget &b,bool input) {
  using V=typename Grid::ValueType;using E=Element<V>;
  for(size_t c=0;c<components<V>;++c) lo[c]=hi[c]=double(component(g.background(),c));
  const auto visit=[&](const V &v) {
    for(size_t c=0;c<components<V>;++c) {
      const auto x=component(v,c);
      if(!std::isfinite(x) || (input && std::abs(x)>std::numeric_limits<E>::max()/E(64))) throw std::invalid_argument("field advection values exceed finite native arithmetic headroom");
      lo[c]=std::min(lo[c],double(x));hi[c]=std::max(hi[c],double(x));
    }
  };
  visit(g.background());for(auto it=g.cbeginValueAll();it;++it) {b.items(1);visit(V(*it));}
}
template<class Grid> struct DoubleVelocityFetch {
  using ValueType=Vec;
  const Grid &grid;
  Vec getValue(const openvdb::Coord &c) const {const auto v=grid.tree().getValue(c);return Vec(v[0],v[1],v[2]);}
};
template<class Grid> Vec velocityBox(const Grid &grid,const Vec &position,int componentOnly=-1) {
  const auto base=openvdb::Coord::floor(position);const Vec fraction=position-base.asVec3d();
  Vec data[2][2][2];openvdb::tools::BoxSampler::getValues(data,DoubleVelocityFetch<Grid>{grid},base);
  const auto result=openvdb::tools::BoxSampler::trilinearInterpolation(data,fraction);
  // Float velocity must be promoted before interpolation. For Double, also
  // reject detectable interpolation underflow before world/index conversion
  // can amplify a lost nonzero component into a representable displacement.
  for(size_t c=0;c<3;++c) if((componentOnly<0 || size_t(componentOnly)==c) && result[c]==0) {
    double scale=0,smallestWeight=1;
    for(int x=0;x<2;++x) for(int y=0;y<2;++y) for(int z=0;z<2;++z) {
      scale=std::max(scale,std::abs(data[x][y][z][c]));
      const double w=(x?fraction.x():1-fraction.x())*(y?fraction.y():1-fraction.y())*(z?fraction.z():1-fraction.z());
      if(w!=0) smallestWeight=std::min(smallestWeight,w);
    }
    if(scale!=0 && scale*smallestWeight==0) {
      double normalized[2][2][2];
      for(int x=0;x<2;++x) for(int y=0;y<2;++y) for(int z=0;z<2;++z) normalized[x][y][z]=data[x][y][z][c]/scale;
      if(openvdb::tools::BoxSampler::trilinearInterpolation(normalized,fraction)!=0)
        throw std::invalid_argument("nonzero Double velocity interpolation underflows before world/index conversion; rescale velocity/time");
    }
  }
  return result;
}
struct Flow {
  Affine source,velocity;
  bool mac{},constant{};Vec constantWorld;double maximumWorld{};
  std::function<Vec(const Vec&)> native;
  Budget &budget;
  uint64_t roundedComponents{};double rounding{};
  template<class Grid> Flow(const Affine &s,const Grid &g,const FieldAdvectionOptions &o,Budget &b):source(s),velocity(g),budget(b) {
    semantics(g);mac=g.getGridClass()==openvdb::GRID_STAGGERED;
    if((o.velocityLayout==FieldVelocityLayout::CollocatedWorld && mac) || (o.velocityLayout==FieldVelocityLayout::NativeMacIndex && !mac)) throw std::invalid_argument("field advection velocity layout disagrees with its native class");
    if(mac) {
      const auto layout=g.template getMetadata<openvdb::StringMetadata>("container:mac_layout"),units=g.template getMetadata<openvdb::StringMetadata>("container:velocity_units");
      if(g.isInWorldSpace() || g.getVectorType()!=openvdb::VEC_CONTRAVARIANT_RELATIVE || !layout || layout->value()!="negative_faces" || !units || units->value()!="voxels_per_second") throw std::invalid_argument("native MAC advection requires local negative_faces/voxels_per_second contravariant-relative tags");
      (void)uniformMap(g);
    } else if(!g.isInWorldSpace() || g.getGridClass()!=openvdb::GRID_UNKNOWN || (g.getVectorType()!=openvdb::VEC_INVARIANT && g.getVectorType()!=openvdb::VEC_CONTRAVARIANT_RELATIVE)) throw std::invalid_argument("collocated advection velocity requires UNKNOWN world invariant/relative semantics");
    Vec envelope(0);constant=true;
    const auto inspect=[&](const typename Grid::ValueType &v) {
      if(!finite(v)) throw std::invalid_argument("nonfinite stored/background advection velocity");
      Vec d(v[0],v[1],v[2]);for(size_t a=0;a<3;++a) envelope[a]=std::max(envelope[a],std::abs(d[a]));
      if(v!=g.background()) constant=false;
      if(!mac) {for(size_t a=0;a<3;++a) if(std::abs(d[a])>1e12) throw std::invalid_argument("world velocity components must be <=1e12");maximumWorld=std::max(maximumWorld,std::hypot(d.x(),d.y(),d.z()));}
    };
    inspect(g.background());for(auto it=g.cbeginValueAll();it;++it) {b.items(1);inspect(typename Grid::ValueType(*it));}
    Vec bg(g.background()[0],g.background()[1],g.background()[2]);constantWorld=mac?velocity.vector(bg):bg;
    if(mac) {
      Vec worldEnvelope(0);
      for(size_t c=0;c<3;++c) {Sum sum;for(size_t a=0;a<3;++a) sum.product(std::abs(velocity.basis[a][c]),envelope[a]);worldEnvelope[c]=sum.get();if(worldEnvelope[c]>1e12) throw std::invalid_argument("native MAC reconstructed world velocity envelope exceeds1e12");}
      maximumWorld=std::hypot(worldEnvelope.x(),worldEnvelope.y(),worldEnvelope.z());
    }
    if(constant) maximumWorld=std::hypot(constantWorld.x(),constantWorld.y(),constantWorld.z());
    else maximumWorld*=sizeof(typename Grid::ValueType::ValueType)==4?1.000001:1.000000000001;
    native=[&g,staggered=mac](const Vec &index) {
      if(!staggered) return velocityBox(g,index);
      Vec value;
      for(size_t a=0;a<3;++a) {Vec p=index;p[a]+=.5;value[a]=velocityBox(g,p,int(a))[a];}
      return value;
    };
  }
  void preflight(const openvdb::CoordBBox &box,double stageTravel) {
    if(constant) return;
    double mapNorm=0,inverseNorm=0;
    for(size_t c=0;c<3;++c) {
      double row=0;for(size_t a=0;a<3;++a) row+=std::abs(velocity.basis[a][c]);mapNorm=std::max(mapNorm,row);
      row=0;for(size_t a=0;a<3;++a) row+=std::abs(velocity.inverse[c][a]);inverseNorm=std::max(inverseNorm,row);
    }
    const double condition=std::max(1.,mapNorm*inverseNorm);
    for(int corner=0;corner<8;++corner) {
      Vec p;for(size_t a=0;a<3;++a) p[a]=(corner&(1<<a)?box.max()[a]+stageTravel+2:box.min()[a]-stageTravel-2);
      double deltaMagnitude=0;
      for(size_t c=0;c<3;++c) {
        double magnitude=std::abs(source.origin[c]-velocity.origin[c]);
        for(size_t a=0;a<3;++a) magnitude+=std::abs(source.basis[a][c]*p[a]);deltaMagnitude=std::max(deltaMagnitude,magnitude);
      }
      const double uncertainty=128*std::numeric_limits<double>::epsilon()*condition*inverseNorm*deltaMagnitude;
      if(!std::isfinite(uncertainty) || uncertainty>1e-4)
        throw std::invalid_argument("field advection joint-map arithmetic uncertainty exceeds 1e-4 velocity voxel; recenter/resample/condition velocity map");
      const auto v=velocity.relativeIndex(source,p);
      for(size_t a=0;a<3;++a) {
        const auto spacing=std::nextafter(v[a],std::numeric_limits<double>::infinity())-v[a];
        if(!std::isfinite(v[a]) || v[a]<INT32_MIN+4. || v[a]>INT32_MAX-4. || !std::isfinite(spacing) || spacing>1e-4) throw std::invalid_argument("field advection full velocity/MAC sampling halo exceeds coordinate/Double precision limits; recenter/resample");
      }
    }
  }
  Vec sample(const Vec &index) {
    budget.items(constant?1:(mac?24:8));Vec world=constantWorld;
    if(!constant) {
      const auto position=velocity.relativeIndex(source,index);
      for(size_t a=0;a<3;++a) if(!std::isfinite(position[a]) || position[a]<INT32_MIN+3. || position[a]>INT32_MAX-3.) throw std::invalid_argument("advection velocity trajectory exceeds safe coordinates");
      world=native(position);if(mac) world=velocity.vector(world);
    }
    const auto result=source.indexVector(world);for(size_t a=0;a<3;++a) if(!std::isfinite(result[a])) throw std::invalid_argument("nonfinite source-index velocity");return result;
  }
};
Vec increment(Flow &flow,const Vec &point,const Vec &v,double dt) {
  Vec out;bool nonzero=false,moved=false;
  for(size_t a=0;a<3;++a) {
    const double d=v[a]*dt;if(v[a]!=0 && dt!=0 && d==0) throw std::invalid_argument("nonzero advection displacement underflows Double");
    out[a]=point[a]+d;
    if(!std::isfinite(d) || !std::isfinite(out[a])) throw std::invalid_argument("advection trajectory overflows Double");
    const double error=std::abs((out[a]-point[a])-d);if(error>1e-4) throw std::invalid_argument("advection trajectory endpoint exceeds coordinate precision envelope");
    flow.rounding=std::max(flow.rounding,error);nonzero|=d!=0;moved|=out[a]!=point[a];if(d!=0 && out[a]==point[a]) ++flow.roundedComponents;
  }
  if(flow.constant && nonzero && !moved) throw std::invalid_argument("advection trajectory cannot resolve its constant nonzero vector displacement; recenter/rescale");return out;
}
Vec trace(Flow &f,const Vec &p,double dt,FieldAdvectionScheme scheme) {
  const auto v0=f.sample(p);
  if(scheme==FieldAdvectionScheme::Semi || correctionScheme(scheme)) return increment(f,p,v0,dt);
  if(dt!=0 && dt*.5==0) throw std::invalid_argument("advection midpoint time underflows Double");
  const auto v1=f.sample(increment(f,p,v0,dt*.5));
  if(scheme==FieldAdvectionScheme::Midpoint) return increment(f,p,v1,dt);
  if(scheme==FieldAdvectionScheme::Rk3) {
    const auto v2=f.sample(increment(f,p,2*v1-v0,dt));return increment(f,p,(v0+4*v1+v2)/6.,dt);
  }
  const auto v2=f.sample(increment(f,p,v1,dt*.5)),v3=f.sample(increment(f,p,v2,dt));return increment(f,p,(v0+2*(v1+v2)+v3)/6.,dt);
}
openvdb::CoordBBox expanded(const openvdb::CoordBBox &box,uint32_t amount) {
  if(box.empty()) return box;openvdb::Coord lo,hi;
  for(size_t a=0;a<3;++a) {
    const int64_t low=int64_t(box.min()[a])-amount,high=int64_t(box.max()[a])+amount;
    if(low<INT32_MIN+4ll || high>INT32_MAX-4ll) throw std::invalid_argument("field advection growth/halo exceeds safe coordinates");lo[a]=int32_t(low);hi[a]=int32_t(high);
  } return {lo,hi};
}
openvdb::CoordBBox shrunk(const openvdb::CoordBBox &box,uint32_t amount) {
  openvdb::Coord lo,hi;for(size_t a=0;a<3;++a) {lo[a]=box.min()[a]+int32_t(amount);hi[a]=box.max()[a]-int32_t(amount);}return {lo,hi};
}
template<class Function> void visit(const openvdb::CoordBBox &box,Budget &b,Function fn) {
  if(box.empty()) return;
  for(int64_t x=box.min().x();x<=box.max().x();++x) for(int64_t y=box.min().y();y<=box.max().y();++y) for(int64_t z=box.min().z();z<=box.max().z();++z) {b.items(1);fn(openvdb::Coord(int32_t(x),int32_t(y),int32_t(z)));}
}
template<class Value> struct Stage {
  openvdb::CoordBBox box,valid;std::array<size_t,3> dimensions{};std::vector<Value> values;std::vector<uint8_t> active;
  explicit Stage(const openvdb::CoordBBox &b):box(b),valid(b) {
    size_t n=1;for(size_t a=0;a<3;++a) {dimensions[a]=size_t(int64_t(b.max()[a])-b.min()[a]+1);n*=dimensions[a];}values.resize(n);active.resize(n);
  }
  size_t offset(const openvdb::Coord &c) const {return ((size_t(int64_t(c.x())-box.min().x())*dimensions[1])+size_t(int64_t(c.y())-box.min().y()))*dimensions[2]+size_t(int64_t(c.z())-box.min().z());}
  std::pair<Value,bool> sample(const Vec &index,Budget &b,Value *minimum=nullptr,Value *maximum=nullptr) const {
    openvdb::Coord base;Vec fraction;
    for(size_t a=0;a<3;++a) {
      if(!std::isfinite(index[a]) || index[a]<INT32_MIN+3. || index[a]>INT32_MAX-3.) throw std::invalid_argument("field advection departure exceeds native coordinates");
      base[a]=int32_t(std::floor(index[a]));fraction[a]=index[a]-base[a];
    }
    Value data[2][2][2];bool support=false;
    for(int x=0;x<2;++x) for(int y=0;y<2;++y) for(int z=0;z<2;++z) {
      b.items(1);const openvdb::Coord c=base+openvdb::Coord(x,y,z);
      if(!valid.isInside(c)) throw std::runtime_error("field advection correction/pass halo was not fully reserved");
      const auto n=offset(c);data[x][y][z]=values[n];
      const double weight=(x?fraction.x():1-fraction.x())*(y?fraction.y():1-fraction.y())*(z?fraction.z():1-fraction.z());
      support=support || (weight!=0 && active[n]);
    }
    if(minimum && maximum) {
      *minimum=*maximum=data[0][0][0];for(int x=0;x<2;++x) for(int y=0;y<2;++y) for(int z=0;z<2;++z) for(size_t c=0;c<components<Value>;++c) {
        assign(*minimum,c,std::min(component(*minimum,c),component(data[x][y][z],c)));assign(*maximum,c,std::max(component(*maximum,c),component(data[x][y][z],c)));
      }
    }
    auto result=openvdb::tools::BoxSampler::trilinearInterpolation(data,fraction);
    if(!finite(result)) throw std::invalid_argument("nonfinite native passive-field interpolation");return {result,support};
  }
};
template<class Value> void pass(const Stage<Value> &input,Stage<Value> &output,Flow &flow,double dt,
    FieldAdvectionScheme scheme,uint32_t radius,Budget &b) {
  output.valid=shrunk(input.valid,radius);
  visit(output.valid,b,[&](const openvdb::Coord &c) {
    const auto departure=trace(flow,c.asVec3d(),-dt,scheme);
    const auto [value,active]=input.sample(departure,b);const auto n=output.offset(c);output.values[n]=value;output.active[n]=uint8_t(active);
  });
}
template<class Value> Value difference(const Value &a,const Value &back) {
  Value result{};using E=Element<Value>;
  for(size_t c=0;c<components<Value>;++c) assign(result,c,E(E(.5)*E(component(a,c)-component(back,c))));return result;
}
template<class Value> Value corrected(const Value &forward,const Value &error) {
  Value result{};for(size_t c=0;c<components<Value>;++c) assign(result,c,Element<Value>(component(forward,c)+component(error,c)));
  if(!finite(result)) throw std::invalid_argument("native field advection error correction overflows");return result;
}
template<class Value> bool nonzero(const Value &v) {for(size_t c=0;c<components<Value>;++c) if(component(v,c)!=0) return true;return false;}
bool selected(const openvdb::Coord &c,const openvdb::CoordBBox &output,const openvdb::MaskGrid *mask) {return output.isInside(c) && (!mask || mask->tree().isValueOn(c));}
template<class Value> void limit(Stage<Value> &candidate,const Stage<Value> &input,const Stage<Value> &forward,
    Flow &flow,double dt,const FieldAdvectionOptions &o,const openvdb::CoordBBox &output,const openvdb::MaskGrid *mask,
    FieldAdvectionResult &report,Budget &b) {
  visit(candidate.valid,b,[&](const openvdb::Coord &c) {
    Value lo{},hi{};const auto departure=trace(flow,c.asVec3d(),-dt,FieldAdvectionScheme::Semi);
    (void)input.sample(departure,b,&lo,&hi);const auto n=candidate.offset(c);auto &value=candidate.values[n];
    bool outside=false,changed=false,allReverted=true;
    for(size_t channel=0;channel<components<Value>;++channel) {
      const auto v=component(value,channel),minimum=component(lo,channel),maximum=component(hi,channel);
      const bool bad=v<minimum || v>maximum;outside|=bad;
      if(bad && o.limiter==FieldAdvectionLimiter::Clamp) {assign(value,channel,std::clamp(v,minimum,maximum));changed=true;}
      else if(bad && o.limiter==FieldAdvectionLimiter::Revert) {assign(value,channel,component(forward.values[n],channel));changed=true;}
      else if(component(value,channel)!=component(forward.values[n],channel)) allReverted=false;
    }
    if(o.limiter==FieldAdvectionLimiter::Revert && allReverted) candidate.active[n]=forward.active[n];
    if(selected(c,output,mask)) {
      report.overshootVoxelCount+=outside;
      if(changed && o.limiter==FieldAdvectionLimiter::Clamp) ++report.clampedVoxelCount;
      if(changed && o.limiter==FieldAdvectionLimiter::Revert) ++report.revertedVoxelCount;
    }
  });
}
template<class Value> void integrate(Stage<Value> &current,Flow &flow,double dt,uint32_t radius,
    const FieldAdvectionOptions &o,const openvdb::CoordBBox &output,const openvdb::MaskGrid *mask,FieldAdvectionResult &report,Budget &b) {
  Stage<Value> forward(current.box);
  if(!correctionScheme(o.scheme)) {
    for(uint32_t step=0;step<o.steps;++step) {pass(current,forward,flow,dt,o.scheme,radius,b);std::swap(current,forward);++report.executedSteps;}
    return;
  }
  Stage<Value> backward(current.box);
  std::optional<Stage<Value>> intermediate;
  if(o.scheme==FieldAdvectionScheme::Bfecc) intermediate.emplace(current.box);
  for(uint32_t step=0;step<o.steps;++step) {
    pass(current,forward,flow,dt,FieldAdvectionScheme::Semi,radius,b);
    pass(forward,backward,flow,-dt,FieldAdvectionScheme::Semi,radius,b);
    if(o.scheme==FieldAdvectionScheme::MacCormack) {
      visit(backward.valid,b,[&](const openvdb::Coord &c) {
        const auto n=current.offset(c);const auto error=difference(current.values[n],backward.values[n]);
        if(selected(c,output,mask)) for(size_t a=0;a<components<Value>;++a) report.maximumRoundTripCorrection[a]=std::max(report.maximumRoundTripCorrection[a],std::abs(double(component(error,a))));
        const bool support=forward.active[n] || (nonzero(error) && (current.active[n] || backward.active[n]));
        backward.values[n]=corrected(forward.values[n],error);backward.active[n]=uint8_t(support);
      });
    } else {
      intermediate->valid=backward.valid;
      visit(intermediate->valid,b,[&](const openvdb::Coord &c) {
        const auto n=current.offset(c);const auto error=difference(current.values[n],backward.values[n]);
        if(selected(c,output,mask)) for(size_t a=0;a<components<Value>;++a) report.maximumRoundTripCorrection[a]=std::max(report.maximumRoundTripCorrection[a],std::abs(double(component(error,a))));
        intermediate->values[n]=corrected(current.values[n],error);intermediate->active[n]=uint8_t(current.active[n] || (nonzero(error) && backward.active[n]));
      });
      pass(*intermediate,backward,flow,dt,FieldAdvectionScheme::Semi,radius,b);
    }
    limit(backward,current,forward,flow,dt,o,output,mask,report,b);std::swap(current,backward);++report.executedSteps;
  }
}
uintmax_t boxCells(const openvdb::CoordBBox &box,uintmax_t cap) {
  if(box.empty()) return 0;uintmax_t n=1;
  for(size_t a=0;a<3;++a) n=mul(n,uint64_t(int64_t(box.max()[a])-box.min()[a]+1),cap,"field advection dense halo exceeds voxel/work cap");return n;
}
uintmax_t leafSlots(const openvdb::CoordBBox &box,uintmax_t cap) {
  uintmax_t n=512;const auto floor=[](int64_t x){return x>=0?x/8:-((-x+7)/8);};
  for(size_t a=0;a<3;++a) n=mul(n,uint64_t(floor(box.max()[a])-floor(box.min()[a])+1),cap,"field advection output leaf slots exceed voxel cap");return n;
}
template<class Grid> FieldAdvectionResult run(const Grid &source,const openvdb::GridBase &velocity,
    const openvdb::MaskGrid *mask,const openvdb::MetaMap &fileMeta,const std::filesystem::path &output,
    const std::filesystem::path &privateOutput,const FieldAdvectionOptions &o,Budget &b) {
  using Value=typename Grid::ValueType;
  values(source,b,true);
  if(source.getVectorType()!=openvdb::VEC_INVARIANT && source.getVectorType()!=openvdb::VEC_CONTRAVARIANT_RELATIVE)
    throw std::invalid_argument("passive field advection supports invariant/relative world channels only");
  const double h=uniformMap(source);Affine sourceMap(source);FieldAdvectionResult report;report.inputActiveVoxelCount=source.activeVoxelCount();report.maskUsed=mask!=nullptr;
  range(source,report.inputMinimum,report.inputMaximum,b,true);
  const bool fog=source.getGridClass()==openvdb::GRID_FOG_VOLUME;
  if(fog && report.inputMinimum[0]<0) throw std::invalid_argument("passive fog advection requires nonnegative background/stored values");
  std::optional<Flow> flow;
  if(velocity.isType<openvdb::Vec3SGrid>()) flow.emplace(sourceMap,static_cast<const openvdb::Vec3SGrid&>(velocity),o,b);
  else if(velocity.isType<openvdb::Vec3DGrid>()) flow.emplace(sourceMap,static_cast<const openvdb::Vec3DGrid&>(velocity),o,b);
  else throw std::invalid_argument("field advection requires Vec3S/Vec3D velocity");
  report.maximumWorldSpeed=flow->maximumWorld;
  bool noOp=o.timeStep==0 || flow->maximumWorld==0 || source.activeVoxelCount()==0 || (mask && mask->activeVoxelCount()==0);
  typename Grid::Ptr result;
  if(!noOp) {
    const double total=flow->maximumWorld*std::abs(o.timeStep)/h,step=total/o.steps,dt=o.timeStep/o.steps;
    if(!std::isfinite(total) || total>256 || step>o.maxCfl) throw std::invalid_argument("field advection total travel must be<=256 voxels and per-step travel<=maxCfl; increase steps/reduce time");
    if(total==0 || step==0 || dt==0) throw std::invalid_argument("nonzero field advection time/displacement underflows integration precision");
    report.maximumStepDisplacement=step;
    const uint32_t passes=o.scheme==FieldAdvectionScheme::Bfecc?3:o.scheme==FieldAdvectionScheme::MacCormack?2:1;
    const uint32_t radius=uint32_t(std::ceil(step))+1,totalHalo=radius*passes*o.steps;
    const auto active=source.evalActiveVoxelBoundingBox(),outBox=expanded(active,totalHalo),workBox=expanded(outBox,totalHalo);
    if(mask) {
      bool any=false;for(auto it=mask->cbeginValueOn();it;++it) {b.items(1);auto overlap=it.getBoundingBox();overlap.intersect(outBox);if(!overlap.empty()) {any=true;break;}}
      noOp=!any;
    }
    if(!noOp) {
      if(source.tree().hasActiveTiles() && !o.voxelizeActiveTiles) throw std::invalid_argument("passive active tiles require explicit voxelizeActiveTiles");
      domain(workBox,0,b);const auto n=boxCells(workBox,b.p.maxVoxelCount);
      const uint64_t fetchCost=flow->constant?1:flow->mac?24:8;
      const uint64_t rkStages=o.scheme==FieldAdvectionScheme::Midpoint?2:o.scheme==FieldAdvectionScheme::Rk3?3:o.scheme==FieldAdvectionScheme::Rk4?4:1;
      const uint64_t stepWork=correctionScheme(o.scheme)?passes*(9+fetchCost)+1+9+fetchCost:9+rkStages*fetchCost;
      uintmax_t remainingWork=mul(n,2+uint64_t(o.steps)*stepWork,o.maxWorkItems,"field advection full pass/correction work exceeds cap");
      add(remainingWork,b.resident,o.maxWorkItems,"field advection final native traversal exceeds work cap");
      add(remainingWork,b.projected,o.maxWorkItems,"field advection final native traversal exceeds work cap");
      if(remainingWork>o.maxWorkItems-b.work) throw std::invalid_argument("field advection shared preflight/pass work exceeds maxWorkItems");
      const auto arrays=correctionScheme(o.scheme)?(o.scheme==FieldAdvectionScheme::Bfecc?4:3):2;
      const auto arrayBytes=mul(mul(n,sizeof(Value)+sizeof(uint8_t),b.p.maxInputBytes,"field advection array exceeds byte cap"),arrays,b.p.maxInputBytes,"field advection correction arrays exceed byte cap");
      b.check(arrayBytes);
      uintmax_t outputSlots=source.tree().leafCount()*512;add(outputSlots,leafSlots(outBox,b.p.maxVoxelCount),b.p.maxVoxelCount,"original+prospective advection output leaf slots exceed voxel cap");
      (void)boxCells(outBox,b.p.maxVoxelCount);
      flow->preflight(workBox,o.scheme==FieldAdvectionScheme::Rk3?3*step:step);
      Stage<Value> current(workBox);
      visit(workBox,b,[&](const openvdb::Coord &c) {const auto index=current.offset(c);Value value;current.active[index]=uint8_t(source.tree().probeValue(c,value));current.values[index]=value;});
      integrate(current,*flow,dt,radius,o,outBox,mask,report,b);
      if(!current.valid.isInside(outBox)) throw std::runtime_error("field advection final support halo was not complete");
      result=detail::DeepCopyGridExact(source);
      visit(outBox,b,[&](const openvdb::Coord &c) {
        if(mask && !mask->tree().isValueOn(c)) return;
        const auto n=current.offset(c);const bool original=source.tree().isValueOn(c),now=current.active[n]!=0;
        if(!original && !now) return;
        if(now) result->tree().setValue(c,current.values[n]);else result->tree().setValueOff(c,current.values[n]);++report.updatedVoxelCount;
      });
      if(result->activeVoxelCount()>b.p.maxVoxelCount || result->tree().leafCount()*512>b.p.maxVoxelCount || result->memUsage()>b.resident+b.projected)
        throw std::runtime_error("field advection output exceeded preflight topology/tree cap");
    }
  }
  if(noOp) result=detail::DeepCopyGridExact(source);
  report.noOp=noOp;range(*result,report.outputMinimum,report.outputMaximum,b,false);
  if(!noOp)detail::RemoveBoundaryProjectionMetadata(*result);
  if(fog && report.outputMinimum[0]<0) {result->setGridClass(openvdb::GRID_UNKNOWN);report.fogClassCleared=true;}
  result->setTransform(detail::CopyTransformExact(source.transform()));result->setName(b.p.gridName);result->setSaveFloatAsHalf(false);
  for(const auto key:{openvdb::GridBase::META_FILE_BBOX_MIN,openvdb::GridBase::META_FILE_BBOX_MAX,openvdb::GridBase::META_FILE_MEM_BYTES,openvdb::GridBase::META_FILE_VOXEL_COUNT}) result->removeMeta(key);
  result->removeMeta("container:field_advection");result->insertMeta("container:field_advection",openvdb::StringMetadata("bounded local-index Double tracing/native trilinear channels"));
  report.grid={result->getName(),result->getGridClass()==openvdb::GRID_FOG_VOLUME?"fog volume":"unknown",result->activeVoxelCount(),h};report.workItems=b.work;
  report.roundedTraceComponentCount=flow->roundedComponents;report.maximumTraceRounding=flow->rounding;
  report.diagnostics.emplace_back("adapted VolumeAdvection schemes with native trilinear field/velocity interpolation, Double local-index trajectories and per-component correction limiters");
  report.diagnostics.emplace_back("conservative weighted-footprint support; no approximate-background pruning; original inactive/unselected values/activity retained outside affected support");
  report.diagnostics.emplace_back("mask selects final writes only; every forward/reverse/correction pass retains its reserved full halo");
  report.diagnostics.emplace_back("overshoot/clamp/revert counts sum voxel-steps; round-trip correction is heuristic; passive transport is not mass conserving");
  if(flow->mac) report.diagnostics.emplace_back("native negative-face MAC +0.5 component reconstruction, index voxels/second converted through J to world velocity");
  if(report.fogClassCleared) report.diagnostics.emplace_back("negative unrestricted-correction density cleared FOG_VOLUME class to UNKNOWN");
  if(noOp) report.diagnostics.emplace_back("exact native value/activity/map no-op; no integration or tile expansion performed");
  b.check();openvdb::io::File writer(privateOutput.string());writer.setCompression(openvdb::io::COMPRESS_ZIP);writer.write(openvdb::GridPtrVec{result},fileMeta);writer.close();copy(privateOutput,output,b.p.maxInputBytes);return report;
}
#endif
} // namespace
FieldAdvectionResult WriteAdvectedField(const std::filesystem::path &input,const std::filesystem::path &velocity,
    const std::filesystem::path &output,const FieldAdvectionOptions &o,const std::string &selection,const std::string &velocitySelection,const ProcessingOptions &p) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  validate(o,p,output);openvdb::initialize();tbb::task_arena arena(1);return arena.execute([&] {
    FastSweepingOptions work;work.maxWorkItems=o.maxWorkItems;work.iterations=o.steps;Budget budget{p,work};Temporary temp;openvdb::MetaMap fileMeta;
    auto source=load(input,temp.directory/"source.vdb",selection,Role::Field,budget,&fileMeta),flow=loadVelocity(velocity,temp.directory/"field.vdb",velocitySelection,budget);
    openvdb::MaskGrid::Ptr mask;
    if(!o.mask.empty()) {const auto loaded=load(o.mask,temp.directory/"mask.vdb",o.maskGrid,Role::Mask,budget);sameMap(*source,*loaded);mask=activityMask(*loaded);}
    if(source->isType<openvdb::FloatGrid>()) return run(static_cast<const openvdb::FloatGrid&>(*source),*flow,mask.get(),fileMeta,output,temp.directory/"result.vdb",o,budget);
    if(source->isType<openvdb::DoubleGrid>()) return run(static_cast<const openvdb::DoubleGrid&>(*source),*flow,mask.get(),fileMeta,output,temp.directory/"result.vdb",o,budget);
    if(source->isType<openvdb::Vec3SGrid>()) return run(static_cast<const openvdb::Vec3SGrid&>(*source),*flow,mask.get(),fileMeta,output,temp.directory/"result.vdb",o,budget);
    return run(static_cast<const openvdb::Vec3DGrid&>(*source),*flow,mask.get(),fileMeta,output,temp.directory/"result.vdb",o,budget);
  });
#else
  (void)input;(void)velocity;(void)output;(void)o;(void)selection;(void)velocitySelection;(void)p;
  throw std::runtime_error("OpenVDB field advection is unavailable; configure with -DENABLE_OPENVDB=ON.");
#endif
}
} // namespace container::geometry::vdb
