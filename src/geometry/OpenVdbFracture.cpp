#include "Container/geometry/OpenVdbFracture.h"
#include "OpenVdbProvenance.h"
#include <stdexcept>
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
#include "OpenVdbFastSweepingInternal.h"
#include <openvdb/tools/Composite.h>
#include <openvdb/tools/ChangeBackground.h>
#include <openvdb/tools/Interpolation.h>
#include <openvdb/tools/LevelSetUtil.h>
#include <iomanip>
#include <numbers>
#include <sstream>
#endif
namespace container::geometry::vdb {
namespace {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
using namespace fast_detail;
struct Rigid {
  openvdb::Mat4d matrix=openvdb::Mat4d::identity(),inverse=openvdb::Mat4d::identity();
  bool identity=true,integerTranslation=true;
  openvdb::Coord shift{0};
};
void validate(const FractureOptions &o,const ProcessingOptions &p,const std::filesystem::path &output) {
  if(!p.maxInputBytes || !p.maxVoxelCount || p.gridName.empty() || p.gridName.size()>220 || p.gridName.find('\0')!=std::string::npos ||
      !o.maxPieces || o.maxPieces>256 || o.instances.size()>32 || !o.maxWorkItems || o.maxWorkItems>10'000'000'000ull)
    throw std::invalid_argument("invalid fracture name/instance/piece/work/byte/voxel limits");
  if(output.empty() || std::filesystem::exists(output) || !std::filesystem::is_directory(output.has_parent_path()?output.parent_path():std::filesystem::path(".")))
    throw std::invalid_argument("fracture requires a new output in an existing directory");
  for(const auto &i:o.instances) {
    for(size_t a=0;a<3;++a) {
      if(!std::isfinite(i.translation[a]) || !std::isfinite(i.rotation[a]) || std::abs(i.rotation[a])>2*std::numbers::pi ||
          (i.pivot && !std::isfinite((*i.pivot)[a]))) throw std::invalid_argument("fracture world translations/pivots must be finite; rotations must be in [-2pi,2pi]");
    }
  }
}
Rigid rigid(const openvdb::GridBase &source,const FractureInstance &i) {
  Rigid out;openvdb::Mat4d j=openvdb::Mat4d::identity(),r=openvdb::Mat4d::identity();
  const bool noRotation=std::all_of(i.rotation.begin(),i.rotation.end(),[](double a){return a==0;});
  if(noRotation && std::all_of(i.translation.begin(),i.translation.end(),[](double a){return a==0;})) return out;
  const auto map=source.transform().baseMap();const auto origin=map->applyMap(openvdb::Vec3d(0));
  for(size_t a=0;a<3;++a) {openvdb::Vec3d e(0);e[a]=1;const auto axis=map->applyJacobian(e);for(size_t b=0;b<3;++b) j[a][b]=axis[b];}
  // Divide out the uniform scale before inversion: Mat4::inverse rejects small
  // absolute determinants even when the original scale is well conditioned.
  const auto h=uniformMap(source);for(size_t a=0;a<3;++a) for(size_t b=0;b<3;++b) j[a][b]/=h;
  for(size_t a=0;a<3;++a) r.postRotate(static_cast<openvdb::math::Axis>(a),i.rotation[a]);
  const auto inverseJ=j.inverse();if(!noRotation) out.matrix=j*r*inverseJ;
  const auto pivot=i.pivot?openvdb::Vec3d((*i.pivot)[0],(*i.pivot)[1],(*i.pivot)[2]):origin;
  const auto delta=noRotation?openvdb::Vec3d(0):origin-pivot;
  const auto displacement=(noRotation?openvdb::Vec3d(0):r.transform3x3(delta)-delta)+openvdb::Vec3d(i.translation[0],i.translation[1],i.translation[2]);
  const auto indexShift=inverseJ.transform3x3(displacement)/h;
  for(size_t a=0;a<3;++a) {
    if(!std::isfinite(indexShift[a]) || std::abs(indexShift[a])>INT32_MAX/8) throw std::invalid_argument("fracture instance translation exceeds safe index coordinates");
    out.matrix[3][a]=indexShift[a];out.shift[a]=int32_t(std::round(indexShift[a]));
    if(double(out.shift[a])!=indexShift[a]) out.integerTranslation=false;
    for(size_t b=0;b<3;++b) if(out.matrix[a][b]!=(a==b?1.:0.)) out.integerTranslation=false;
  }
  out.identity=out.matrix==openvdb::Mat4d::identity();out.inverse=out.matrix.inverse();return out;
}
openvdb::CoordBBox transformedBox(const openvdb::CoordBBox &box,const Rigid &r) {
  openvdb::CoordBBox out;
  for(int corner=0;corner<8;++corner) {
    openvdb::Vec3d p;
    for(size_t a=0;a<3;++a) p[a]=corner&(1<<a)?box.max()[a]:box.min()[a];
    const auto q=r.matrix.transformH(p);const auto back=r.inverse.transformH(q);
    openvdb::Coord lo,hi;
    for(size_t a=0;a<3;++a) {
      if(!std::isfinite(q[a]) || std::abs(q[a])>INT32_MAX/8 || std::abs(back[a]-p[a])>1e-6)
        throw std::invalid_argument("fracture rigid transform cannot resolve its source lattice; recenter/partition first");
      lo[a]=int32_t(std::floor(q[a]))-2;hi[a]=int32_t(std::ceil(q[a]))+2;
    } out.expand(lo);out.expand(hi);
  } return out;
}
template<class Function> void cells(const openvdb::CoordBBox &box,Budget &b,Function f) {
  for(int64_t x=box.min().x();x<=box.max().x();++x) for(int64_t y=box.min().y();y<=box.max().y();++y) for(int64_t z=box.min().z();z<=box.max().z();++z) {
    b.items(1);f(openvdb::Coord(int32_t(x),int32_t(y),int32_t(z)));
  }
}
template<class Grid> uint64_t negatives(const Grid &g,Budget &b) {
  uintmax_t count=0;const auto box=g.evalActiveVoxelBoundingBox();
  if(box.empty()) return 0;
  for(auto it=g.cbeginValueAll();it;++it) if(*it<0) {
    b.items(1);auto region=it.getBoundingBox();region.intersect(box);if(region.empty()) continue;
    uintmax_t n=1;for(size_t a=0;a<3;++a) n=mul(n,uint64_t(int64_t(region.max()[a])-region.min()[a]+1),b.p.maxVoxelCount,"fracture interior sample count exceeds cap");
    add(count,n,b.p.maxVoxelCount,"fracture interior sample count exceeds cap");
  } return count;
}
template<class Grid> void closed(const Grid &g,Budget &b,bool allowEmpty=false) {
  using V=typename Grid::ValueType;values(g,b);
  if(g.getGridClass()!=openvdb::GRID_LEVEL_SET || !g.isInWorldSpace() || g.getVectorType()!=openvdb::VEC_INVARIANT ||
      !(g.background()>0) || g.tree().hasActiveTiles()) throw std::invalid_argument("fracture requires world/invariant scalar LEVEL_SET inputs with positive background and no active tiles");
  const auto h=uniformMap(g),width=double(g.background())/h;
  if(width<3 || width>64) throw std::invalid_argument("fracture requires a [3,64]-voxel background width");
  const auto box=g.evalActiveVoxelBoundingBox();
  if(box.empty()) {if(allowEmpty) return;throw std::invalid_argument("fracture input is empty");}
  for(auto it=g.cbeginValueAll();it;++it) {
    b.items(1);
    if(std::abs(V(*it))>g.background()) throw std::invalid_argument("fracture stored distance exceeds its background width");
    if(!it.isValueOn() && std::abs(V(*it))!=g.background()) throw std::invalid_argument("fracture inactive distances must equal native signed background widths");
    if(*it<0 && !box.isInside(it.getBoundingBox())) throw std::invalid_argument("fracture field has a negative exterior/missing active boundary");
  }
  for(size_t axis=0;axis<3;++axis) for(int side=0;side<2;++side) {
    auto face=box;const auto edge=side?box.max()[axis]:box.min()[axis];face.min()[axis]=face.max()[axis]=edge;
    cells(face,b,[&](const openvdb::Coord &c) {if(!(g.tree().getValue(c)>0)) throw std::invalid_argument("fracture requires positive bounding faces; open or negative-exterior/cavity-only surfaces are unsupported");});
  }
  cells(box,b,[&](const openvdb::Coord &c) {
    V value;if(g.tree().probeValue(c,value) || !(value<0)) return;
    for(size_t a=0;a<3;++a) for(int step:{-1,1}) {auto next=c;next[a]+=step;V neighbor;b.items(1);if(!g.tree().probeValue(next,neighbor) && neighbor>=0) throw std::invalid_argument("fracture field has an inactive sign jump without an active narrow band");}
  });
}
template<class Grid> auto instance(const Grid &cutter,const Rigid &r,const openvdb::CoordBBox &box,Budget &b) {
  if(r.identity) return detail::DeepCopyGridExact(cutter);
  auto out=detail::CopyGridWithNewTreeExact(cutter);
  cells(box,b,[&](const openvdb::Coord &c) {
    typename Grid::ValueType value;bool active;
    if(r.integerTranslation) {const auto q=c-r.shift;active=cutter.tree().probeValue(q,value);}
    else active=openvdb::tools::BoxSampler::sample(cutter.tree(),r.inverse.transformH(c.asVec3d()),value);
    if(!std::isfinite(value)) throw std::invalid_argument("fracture cutter interpolation produced a nonfinite value");
    if(active) out->tree().setValue(c,value);
    else if(value<0) out->tree().setValueOff(c,-cutter.background());
  });
  if(out->memUsage()>b.projected || out->tree().leafCount()*512>b.p.maxVoxelCount || out->activeVoxelCount()>b.p.maxVoxelCount)
    throw std::runtime_error("fracture transformed cutter exceeded reserved tree/domain");
  return out;
}
template<class Grid> struct Piece {typename Grid::Ptr grid;bool residual;uint32_t instance;uint64_t serial;};
template<class Grid> uint32_t surfaceCount(const Grid &g,Budget &b,uint32_t cap) {
  auto mask=openvdb::tools::extractIsosurfaceMask(g.tree(),typename Grid::ValueType(0));
  const auto count=mask->activeVoxelCount();if(count>b.p.maxVoxelCount) throw std::invalid_argument("fracture segmentation mask exceeds voxel cap");
  using MaskTree=typename Grid::TreeType::template ValueConverter<bool>::Type;
  using Leaf=typename MaskTree::LeafNodeType;
  uintmax_t fragments=0,squares=0;
  for(auto leaf=mask->cbeginLeaf();leaf;++leaf) {
    auto remaining=leaf->getValueMask();uintmax_t local=0;std::array<openvdb::Index,512> localQueue{};
    while(!remaining.isOff()) {
      ++local;size_t queued=0;const auto first=remaining.findFirstOn();remaining.setOff(first);localQueue[queued++]=first;
      while(queued) {
        const auto c=Leaf::offsetToLocalCoord(localQueue[--queued]);
        for(size_t a=0;a<3;++a) for(int step:{-1,1}) {
          b.items(1);auto next=c;next[a]+=step;if(next[a]<0 || next[a]>=8) continue;
          const auto offset=Leaf::coordToOffset(next);if(remaining.isOn(offset)) {remaining.setOff(offset);localQueue[queued++]=offset;}
        }
      }
    }
    add(fragments,local,b.o.maxWorkItems,"segmentation local fragments exceed work cap");
    add(squares,mul(local,local,b.o.maxWorkItems,"segmentation squared fragments exceed work cap"),b.o.maxWorkItems,"segmentation squared fragments exceed work cap");
  }
  openvdb::tree::ValueAccessor<MaskTree> access(*mask);
  std::vector<openvdb::Coord> queue;queue.reserve(size_t(count));uint32_t components=0;
  for(auto leaf=mask->beginLeaf();leaf;++leaf) for(auto value=leaf->beginValueOn();value;++value) {
    if(++components>cap) throw std::invalid_argument("fracture segmentation would exceed maxPieces");
    queue.clear();queue.push_back(value.getCoord());access.setValueOff(queue.back(),false);
    for(size_t n=0;n<queue.size();++n) for(size_t a=0;a<3;++a) for(int step:{-1,1}) {
      b.items(1);auto next=queue[n];next[a]+=step;if(access.isValueOn(next)) {queue.push_back(next);access.setValueOff(next,false);}
    }
  }
  const auto globalGroups=mul(components,fragments,b.o.maxWorkItems,"segmentation global groups exceed work cap");
  b.items(squares);b.items(globalGroups);
  uintmax_t graph=mul(squares+globalGroups,sizeof(void*),b.p.maxInputBytes,"segmentation graph exceeds byte cap");
  add(graph,mul(count,sizeof(openvdb::Coord),b.p.maxInputBytes,"segmentation queue exceeds byte cap"),b.p.maxInputBytes,"segmentation graph/queue exceeds byte cap");
  b.check(graph);return components;
}
template<class Grid> FractureResult fracture(const Grid &source,const Grid &cutter,const openvdb::MetaMap &fileMeta,
    const std::filesystem::path &output,const std::filesystem::path &privateOutput,const FractureOptions &o,Budget &b,double h) {
  using V=typename Grid::ValueType;
  if(source.background()!=cutter.background()) throw std::invalid_argument("fracture source/cutter native background widths must match exactly");
  auto sourceBox=source.evalActiveVoxelBoundingBox(),cutterBox=cutter.evalActiveVoxelBoundingBox();
  if(sourceBox.empty() || cutterBox.empty()) throw std::invalid_argument("fracture source/cutter active bounds are empty");
  auto global=sourceBox;std::vector<Rigid> transforms;std::vector<openvdb::CoordBBox> boxes;
  const auto instances=o.instances.empty()?std::vector<FractureInstance>{FractureInstance{}}:o.instances;
  uint64_t potential=1;
  for(const auto &i:instances) {
    const auto r=rigid(source,i);const auto box=transformedBox(cutterBox,r);global.expand(box);transforms.push_back(r);boxes.push_back(box);
    potential=o.cutterOverlap?mul(potential,2,o.maxPieces,"fracture worst-case instance pieces exceed maxPieces"):(potential+1);
    if(potential>o.maxPieces) throw std::invalid_argument("fracture worst-case instance pieces exceed maxPieces");
  }
  if(o.segment) potential=o.maxPieces;
  domain(global,2,b);
  uintmax_t alignedSlots=512;
  for(size_t a=0;a<3;++a) {
    const int64_t lo=int64_t(global.min()[a])-4,hi=int64_t(global.max()[a])+4;
    const auto floor=[](int64_t n){return n>=0?n/8:-((-n+7)/8);};
    alignedSlots=mul(alignedSlots,uint64_t(floor(hi)-floor(lo)+1),b.p.maxVoxelCount,"fracture projected leaf slots exceed cap");
  }
  add(alignedSlots,b.slots,b.p.maxVoxelCount,"fracture original/projected leaf slots exceed cap");
  (void)mul(alignedSlots,potential,b.p.maxVoxelCount,"aggregate worst-case fracture leaf slots exceed maxVoxelCount before first cut");
  b.projected=mul(b.projected,potential+1,b.p.maxInputBytes,"aggregate fracture piece trees exceed byte cap");
  add(b.projected,mul(b.metadata+1024,potential+1,b.p.maxInputBytes,"aggregate fracture metadata exceeds cap"),b.p.maxInputBytes,"aggregate fracture trees/metadata exceed cap");b.check();
  b.items(mul(uint64_t(global.volume()),64*(potential+1)*instances.size(),o.maxWorkItems,"aggregate fracture instances/pieces exceed maxWorkItems"));
  closed(source,b);closed(cutter,b);
  const auto originalCount=negatives(source,b);if(!originalCount || !negatives(cutter,b)) throw std::invalid_argument("fracture inputs must contain represented negative interior samples");
  auto initial=detail::DeepCopyGridExact(source);initial->setTransform(openvdb::math::Transform::createLinearTransform(1));
  auto unitCutter=detail::DeepCopyGridExact(cutter);unitCutter->setTransform(openvdb::math::Transform::createLinearTransform(1));
  std::vector<Piece<Grid>> pieces{{initial,true,0,0}};uint64_t serial=0,boundarySamples=0;
  for(uint32_t inst=0;inst<instances.size();++inst) {
    auto tool=instance(*unitCutter,transforms[inst],boxes[inst],b);std::vector<Piece<Grid>> next;
    for(auto &piece:pieces) {
      if(!o.cutterOverlap && !piece.residual) {next.push_back(std::move(piece));continue;}
      b.check();const auto before=negatives(*piece.grid,b);
      auto cut=openvdb::tools::csgIntersectionCopy(*piece.grid,*tool);
      auto residual=openvdb::tools::csgDifferenceCopy(*piece.grid,*tool);
      const auto nc=negatives(*cut,b),nr=negatives(*residual,b);uint64_t ties=0;
      cells(piece.grid->evalActiveVoxelBoundingBox(),b,[&](const openvdb::Coord &c) {if(piece.grid->tree().getValue(c)<0 && tool->tree().getValue(c)==0) ++ties;});
      if(nc+nr+ties!=before) throw std::runtime_error("fracture native negative/cut-boundary sample partition failed");
      add(boundarySamples,ties,b.p.maxVoxelCount,"fracture cut-boundary count exceeds voxel cap");
      for(const auto &g:{cut,residual}) if(g->memUsage()>b.projected || g->activeVoxelCount()>b.p.maxVoxelCount || g->tree().leafCount()*512>b.p.maxVoxelCount)
        throw std::runtime_error("native fracture piece exceeded preflight tree/voxel cap");
      if(nc) next.push_back({cut,false,inst,++serial});
      if(nr) next.push_back({residual,piece.residual,piece.instance,piece.serial});
      if(next.size()>o.maxPieces) throw std::invalid_argument("fracture output count exceeds maxPieces");
    } pieces=std::move(next);
  }
  if(o.segment) {
    std::vector<Piece<Grid>> segmented;
    for(const auto &piece:pieces) {
      const auto count=surfaceCount(*piece.grid,b,o.maxPieces-uint32_t(segmented.size()));
      std::vector<typename Grid::Ptr> parts;openvdb::tools::segmentSDF(*piece.grid,parts);
      if(parts.size()!=count || parts.size()+segmented.size()>o.maxPieces) throw std::runtime_error("native fracture segmentation disagrees with bounded prepass");
      for(auto &part:parts) {
        // Restore world map before closure/band checks; segmentation may copy
        // the unit map safely, but physical widths belong to the source map.
        part->setTransform(detail::CopyTransformExact(source.transform()));
        // Native segmentSDF may retain former active distances as inactive
        // values while extracting a component. Clamp these to signed widths;
        // negative exterior signs survive this and must still fail closure.
        openvdb::tools::changeAsymmetricLevelSetBackground(part->tree(),part->background(),-part->background(),false);
        closed(*part,b);
        segmented.push_back({part,piece.residual,piece.instance,++serial});
      }
    } pieces=std::move(segmented);
  }
  std::stable_sort(pieces.begin(),pieces.end(),[](const auto &a,const auto &c) {
    if(a.residual!=c.residual) return a.residual;
    const auto an=a.grid->activeVoxelCount(),cn=c.grid->activeVoxelCount();if(an!=cn) return an>cn;
    const auto ab=a.grid->evalActiveVoxelBoundingBox(),cb=c.grid->evalActiveVoxelBoundingBox();
    for(size_t k=0;k<6;++k) {const auto av=k<3?ab.min()[k]:ab.max()[k-3],cv=k<3?cb.min()[k]:cb.max()[k-3];if(av!=cv) return av<cv;}
    return a.serial<c.serial;
  });
  FractureResult report;report.inputNegativeVoxelCount=originalCount;report.cutBoundaryVoxelCount=boundarySamples;
  uintmax_t active=0,slots=0,bytes=0;openvdb::GridPtrVec grids;uint32_t residualIndex=0,fragmentIndex=0;
  for(auto &piece:pieces) {
    add(active,piece.grid->activeVoxelCount(),b.p.maxVoxelCount,"summed fracture output activity exceeds voxel cap");
    add(slots,piece.grid->tree().leafCount()*512,b.p.maxVoxelCount,"summed fracture leaf slots exceed voxel cap");
    add(bytes,piece.grid->memUsage(),b.p.maxInputBytes,"summed fracture output trees exceed byte cap");
    std::ostringstream name;name<<b.p.gridName<<(piece.residual?"_residual_":"_fragment_")<<std::setw(3)<<std::setfill('0')<<(piece.residual?residualIndex++:fragmentIndex++);
    piece.grid->insertMeta(source);piece.grid->setTransform(detail::CopyTransformExact(source.transform()));piece.grid->setName(name.str());piece.grid->setSaveFloatAsHalf(false);piece.grid->setGridClass(openvdb::GRID_LEVEL_SET);
    detail::RemoveBoundaryProjectionMetadata(*piece.grid);
    for(const auto key:{openvdb::GridBase::META_FILE_BBOX_MIN,openvdb::GridBase::META_FILE_BBOX_MAX,openvdb::GridBase::META_FILE_MEM_BYTES,openvdb::GridBase::META_FILE_VOXEL_COUNT}) piece.grid->removeMeta(key);
    piece.grid->insertMeta("container:fracture_role",openvdb::StringMetadata(piece.residual?"residual":"fragment"));
    piece.grid->insertMeta("container:fracture_instance",openvdb::Int32Metadata(int32_t(piece.instance)));
    const auto n=negatives(*piece.grid,b);add(report.outputNegativeVoxelCount,n,b.p.maxVoxelCount,"summed fracture interiors exceed cap");
    report.pieces.push_back({{name.str(),"level set",piece.grid->activeVoxelCount(),h},piece.residual,piece.instance,n});grids.push_back(piece.grid);
  }
  if(report.outputNegativeVoxelCount+boundarySamples!=originalCount) throw std::runtime_error("final fracture interior/boundary partition failed");
  b.check();report.workItems=b.work;
  report.diagnostics.emplace_back("native intersection/difference orchestration; represented negative pieces retained without the SDK27-active-voxel discard heuristic");
  report.diagnostics.emplace_back("negative-voxel samples plus new cutter-zero samples exactly partition the input; CSG/rigid-interpolated distances and continuous volumes are approximate");
  report.diagnostics.emplace_back(o.segment?"native surface segmentation; negative-exterior cavity components reject before publication":"nested cavities retained within their original fragment/residual grids; no solid-region labeling inferred");
  report.diagnostics.emplace_back("bounded world-rigid instances use Double per-point inverse sampling; exact source maps/world units/metadata retained; full precision ZIP and exclusive publication");
  openvdb::io::File writer(privateOutput.string());writer.setCompression(openvdb::io::COMPRESS_ZIP);writer.write(grids,fileMeta);writer.close();copy(privateOutput,output,b.p.maxInputBytes);return report;
}
#endif
} // namespace
FractureResult WriteFracturedLevelSet(const std::filesystem::path &input,const std::filesystem::path &cutter,
    const std::filesystem::path &output,const FractureOptions &o,const std::string &selection,const std::string &cutterSelection,const ProcessingOptions &p) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  validate(o,p,output);openvdb::initialize();tbb::task_arena arena(1);return arena.execute([&] {
    FastSweepingOptions limits;limits.maxWorkItems=o.maxWorkItems;limits.iterations=uint32_t(std::max<size_t>(1,o.instances.size()));Budget budget{p,limits};Temporary temp;openvdb::MetaMap fileMeta;
    auto source=load(input,temp.directory/"source.vdb",selection,Role::Scalar,budget,&fileMeta);
    auto tool=load(cutter,temp.directory/"mask.vdb",cutterSelection,Role::Scalar,budget);sameMap(*source,*tool);const auto h=uniformMap(*source);
    if(source->type()!=tool->type()) throw std::invalid_argument("fracture source/cutter native precision must match");
    if(source->isType<openvdb::FloatGrid>()) return fracture(static_cast<const openvdb::FloatGrid&>(*source),static_cast<const openvdb::FloatGrid&>(*tool),fileMeta,output,temp.directory/"result.vdb",o,budget,h);
    return fracture(static_cast<const openvdb::DoubleGrid&>(*source),static_cast<const openvdb::DoubleGrid&>(*tool),fileMeta,output,temp.directory/"result.vdb",o,budget,h);
  });
#else
  (void)input;(void)cutter;(void)output;(void)o;(void)selection;(void)cutterSelection;(void)p;
  throw std::runtime_error("OpenVDB fracture is unavailable; configure with -DENABLE_OPENVDB=ON.");
#endif
}
} // namespace container::geometry::vdb
