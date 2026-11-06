#include "OpenVdbFastSweepingInternal.h"
#include <openvdb/tools/FastSweeping.h>
#include <openvdb/tools/Interpolation.h>
namespace container::geometry::vdb::fast_detail {
namespace {
using Grid=CONTAINER_FAST_SWEEPING_GRID;
using Scalar=typename Grid::ValueType;
inline openvdb::tools::FastSweepingDomain direction(FastSweepingSide side) {
  using D=openvdb::tools::FastSweepingDomain;
  return side==FastSweepingSide::GreaterThanIsovalue?D::SWEEP_GREATER_THAN_ISOVALUE:
      side==FastSweepingSide::LessThanIsovalue?D::SWEEP_LESS_THAN_ISOVALUE:D::SWEEP_ALL;
}
template<class FS> void solve(FS &fs,Budget &b) {
  if(!fs.boundaryVoxelCount()) throw std::invalid_argument("fast sweeping domain has no boundary/iso crossing");
  if(fs.sweepingVoxelCount()) fs.sweep(int(b.o.iterations),false);
  auto sdf=fs.sdfGrid();const Scalar unknown=std::numeric_limits<Scalar>::max();
  for(auto it=sdf->cbeginValueOn();it;++it) {
    b.items(1);if(!std::isfinite(Scalar(*it)) || std::abs(Scalar(*it))>=unknown)
      throw std::invalid_argument("fast sweeping has unresolved active components; connect each domain to a boundary or increase iterations");
  }
  // Native finalization uses the largest/smallest active value as outside/
  // inside widths. A fog domain containing only interior active voxels then
  // throws because its largest distance is negative. Use a symmetric envelope
  // one voxel wider than every resolved active distance, retaining signs of all
  // inactive cells and making the result usable as a subsequent dilation seed.
  Scalar magnitude=0;
  for(auto it=sdf->cbeginValueOn();it;++it) magnitude=std::max(magnitude,std::abs(Scalar(*it)));
  const Scalar envelope=magnitude+Scalar(1);
  if(!std::isfinite(envelope) || !(envelope>magnitude)) throw std::invalid_argument("distance domain exceeds native unit-voxel resolution; partition/recenter first");
  openvdb::tools::changeAsymmetricLevelSetBackground(sdf->tree(),envelope,-envelope,false);
}
inline void scale(Grid &g,double factor,Budget &b) {
  const double maximum=std::numeric_limits<Scalar>::max();
  for(auto it=g.beginValueAll();it;++it) {
    b.items(1);const Scalar before=*it;const double after=double(before)*factor;
    if(!std::isfinite(after) || std::abs(after)>maximum || (before!=0 && Scalar(after)==0))
      throw std::invalid_argument("fast sweeping distance scaling exceeds native precision range");
    it.setValue(Scalar(after));
  }
  const double bg=double(g.background())*factor;
  if(!std::isfinite(bg) || bg>maximum || !(Scalar(bg)>0)) throw std::invalid_argument("fast sweeping background exceeds native precision range");
  g.tree().root().setBackground(Scalar(bg),false);
}
inline void conditionIso(Grid &g,Scalar iso,Budget &b) {
  // Native InitSdf treats |delta| <= Tolerance as exactly on the surface and
  // forms inverse-square interpolation weights. Normalize the scalar range
  // before initialization, then reject unresolved distinctions instead of
  // silently widening tiny-valued fog/SDF interfaces to exact zero voxels.
  const auto cap=std::numeric_limits<Scalar>::max()/4096;
  if(std::abs(iso)>cap) throw std::invalid_argument("isovalue exceeds safe native interpolation range");
  Scalar amplitude=0;
  const auto inspect=[&](Scalar value) {
    if(std::abs(value)>cap) throw std::invalid_argument("scalar values exceed safe native interpolation range");
    amplitude=std::max(amplitude,std::abs(Scalar(value-iso)));
  };
  inspect(g.background());for(auto it=g.cbeginValueAll();it;++it) {b.items(1);inspect(Scalar(*it));}
  if(!(amplitude>0)) throw std::invalid_argument("fast sweeping scalar domain has no iso variation");
  const auto threshold=Scalar(8)*std::max(openvdb::math::Tolerance<Scalar>::value(),std::numeric_limits<Scalar>::epsilon());
  const auto normalized=[&](Scalar value,bool active) {
    const Scalar delta=Scalar(value-iso),n=delta/amplitude;
    if(delta!=0 && (n==0 || (active && std::abs(n)<threshold)))
      throw std::invalid_argument("native iso interpolation cannot resolve this scalar dynamic range; rescale/partition the field");
    return n;
  };
  const auto background=normalized(g.background(),false);
  for(auto it=g.beginValueAll();it;++it) it.setValue(normalized(Scalar(*it),it.isValueOn()));
  g.tree().root().setBackground(background,false);
}
template<class ExtGrid> NativeResult extend(const Grid &working,const Grid &source,const ExtGrid &field,
    bool inputFog,const FastSweepingOptions &o,Budget &b,double h) {
  using Value=typename ExtGrid::ValueType;
  values(field,b,true);
  // Keep interpolation arithmetic away from overflow, including sums and native
  // weighted extension. This is intentionally stricter than mere finiteness.
  const auto threshold=Scalar(8)*std::max(openvdb::math::Tolerance<Scalar>::value(),std::numeric_limits<Scalar>::epsilon());
  const auto bound=std::min(std::numeric_limits<Scalar>::max()/Scalar(1e16),std::numeric_limits<Scalar>::max()*(threshold*threshold/Scalar(256)));
  const auto bounded=[&](const Value &v) {
    if constexpr(openvdb::VecTraits<Value>::IsVec) {for(size_t i=0;i<3;++i) if(std::abs(v[i])>bound) throw std::invalid_argument("extension field values exceed safe native arithmetic range");}
    else if(std::abs(v)>bound) throw std::invalid_argument("extension field values exceed safe native arithmetic range");
  };
  bounded(field.background());for(auto it=field.cbeginValueAll();it;++it) bounded(Value(*it));
  auto sample=[&field](const openvdb::Vec3d &index)->Value {
    auto value=openvdb::tools::BoxSampler::sample(field.tree(),index);
    if(!finite(value)) throw std::invalid_argument("nonfinite native extension boundary interpolation");return value;
  };
  openvdb::tools::FastSweeping<Grid,Value> fs;
  fs.initExt(working,sample,field.background(),Scalar(0),!inputFog);
  solve(fs,b);auto result=fs.extGrid();auto distance=fs.sdfGrid();
  // Native one-sided modes restore excluded computing cells but modify excluded
  // boundary cells. Apply the exact original-value policy consistently instead.
  if(o.side!=FastSweepingSide::All) for(auto it=result->beginValueOn();it;++it) {
    const auto input=source.tree().getValue(it.getCoord());
    const bool selected=o.side==FastSweepingSide::GreaterThanIsovalue?input>Scalar(o.isoValue):input<Scalar(o.isoValue);
    if(!selected) it.setValue(field.tree().getValue(it.getCoord()));
  }
  for(auto it=result->cbeginValueAll();it;++it) {b.items(1);if(!finite(Value(*it))) throw std::runtime_error("nonfinite native extension result");}
  result->insertMeta(field);scale(*distance,h,b);
  return {distance,result,fs.boundaryVoxelCount(),fs.sweepingVoxelCount()};
}
} // namespace
NativeResult CONTAINER_FAST_SWEEPING_RUN(const openvdb::GridBase &base,const openvdb::GridBase *field,
    const openvdb::MaskGrid *mask,FastSweepingOperation operation,bool inputFog,const FastSweepingOptions &o,Budget &b,double h) {
  const auto &source=static_cast<const Grid&>(base);values(source,b);
  if(!source.isInWorldSpace() || source.getVectorType()!=openvdb::VEC_INVARIANT || source.getGridClass()==openvdb::GRID_STAGGERED)
    throw std::invalid_argument("fast sweeping source requires ordinary scalar semantics");
  if(inputFog) {
    if(source.getGridClass()==openvdb::GRID_LEVEL_SET) throw std::invalid_argument("fog conversion rejects a tagged SDF; use reinitialization");
  } else if(source.getGridClass()!=openvdb::GRID_LEVEL_SET || !(source.background()>0))
    throw std::invalid_argument("SDF fast sweeping requires LEVEL_SET class and positive background");
  if(!std::isfinite(Scalar(o.isoValue)) || (o.isoValue!=0 && Scalar(o.isoValue)==0)) throw std::invalid_argument("fast sweeping isovalue exceeds native precision range");
  if(source.tree().hasActiveTiles() && (!inputFog || !o.voxelizeActiveTiles)) throw std::invalid_argument("SDF active tiles are unsupported; fog tiles require explicit voxelization");
  auto box=source.evalActiveVoxelBoundingBox();
  if(mask) box.expand(mask->evalActiveVoxelBoundingBox());
  domain(box,operation==FastSweepingOperation::DilateSdf?o.dilation:0,b);
  auto working=detail::DeepCopyGridExact(source);working->setTransform(openvdb::math::Transform::createLinearTransform(1));
  if(o.voxelizeActiveTiles) working->tree().voxelizeActiveTiles(false);
  if(mask && operation!=FastSweepingOperation::MaskSdf) {
    // topologyIntersection prunes removed branches to the scalar background,
    // changing inactive stencil signs at the restriction boundary. Preserve
    // every logical scalar value while intersecting activity only.
    openvdb::BoolTree selected(mask->tree(),false,true,openvdb::TopologyCopy());
    auto intersected=std::make_shared<Grid::TreeType>(working->background());
    auto kernel=[](openvdb::CombineArgs<Scalar,bool> &args) {
      args.setResult(args.a());args.setResultIsActive(args.aIsActive()&&args.bIsActive());
    };
    intersected->combine2Extended(working->tree(),selected,kernel,false);working->setTree(intersected);
  }
  if(working->activeVoxelCount()==0) throw std::invalid_argument("fast sweeping masked active domain is empty");
  if(operation!=FastSweepingOperation::DilateSdf && operation!=FastSweepingOperation::MaskSdf) conditionIso(*working,Scalar(o.isoValue),b);
  NativeResult native;
  if(field) {
    if(field->isType<Grid>()) native=extend(*working,source,static_cast<const Grid&>(*field),inputFog,o,b,h);
    else if(field->isType<CONTAINER_FAST_SWEEPING_VECTOR_GRID>()) native=extend(*working,source,static_cast<const CONTAINER_FAST_SWEEPING_VECTOR_GRID&>(*field),inputFog,o,b,h);
    else throw std::invalid_argument("extension precision must match scalar source: Float/Vec3S or Double/Vec3D");
  } else {
    openvdb::tools::FastSweeping<Grid> fs;
    if(operation==FastSweepingOperation::DilateSdf || operation==FastSweepingOperation::MaskSdf) {
      for(auto it=source.cbeginValueOn();it;++it) if(std::abs(Scalar(*it))>=source.background())
        throw std::invalid_argument("band/mask extension requires every active seed strictly inside the background width");
      scale(*working,1/h,b);
      if(operation==FastSweepingOperation::DilateSdf) {
        const auto nn=o.neighbors==FastSweepingNeighbors::Face?openvdb::tools::NN_FACE:
            o.neighbors==FastSweepingNeighbors::FaceEdge?openvdb::tools::NN_FACE_EDGE:openvdb::tools::NN_FACE_EDGE_VERTEX;
        fs.initDilate(*working,int(o.dilation),nn,direction(o.side));
      } else {if(!mask) throw std::logic_error("missing native mask");fs.initMask(*working,*mask,false);}
    } else fs.initSdf(*working,Scalar(0),!inputFog);
    solve(fs,b);native={fs.sdfGrid(),{},fs.boundaryVoxelCount(),fs.sweepingVoxelCount()};scale(static_cast<Grid&>(*native.distance),h,b);
    // Preserve native seed bits after temporary world/index scaling.
    if(operation==FastSweepingOperation::DilateSdf || operation==FastSweepingOperation::MaskSdf) {
      auto &tree=static_cast<Grid&>(*native.distance).tree();for(auto it=source.cbeginValueOn();it;++it) tree.setValue(it.getCoord(),Scalar(*it));
    }
  }
  if(native.distance->activeVoxelCount()>b.p.maxVoxelCount || native.distance->baseTree().leafCount()*512>b.p.maxVoxelCount || native.distance->memUsage()>b.projected)
    throw std::runtime_error("native sweeping result exceeded preflight domain/tree limits");
  if(native.extension && (native.extension->activeVoxelCount()!=native.distance->activeVoxelCount() || native.extension->memUsage()>b.projected))
    throw std::runtime_error("native extension result exceeded shared topology/tree limits");
  return native;
}
} // namespace container::geometry::vdb::fast_detail
