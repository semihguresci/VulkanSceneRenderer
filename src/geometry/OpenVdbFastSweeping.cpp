#include "Container/geometry/OpenVdbFastSweeping.h"
#include <stdexcept>
#include <cmath>
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
#include "OpenVdbFastSweepingInternal.h"
#include <openvdb/tools/TopologyToLevelSet.h>
#endif
namespace container::geometry::vdb {
namespace {
[[noreturn]] void unavailable() {throw std::runtime_error("OpenVDB fast sweeping is unavailable; configure with -DENABLE_OPENVDB=ON.");}
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
using namespace fast_detail;
void validate(const FastSweepingOptions &o,const ProcessingOptions &p,FastSweepingOperation operation,bool extension,const std::filesystem::path &output) {
  if(operation<FastSweepingOperation::FogToSdf || operation>FastSweepingOperation::TopologyToSdf ||
      o.side<FastSweepingSide::All || o.side>FastSweepingSide::LessThanIsovalue ||
      o.neighbors<FastSweepingNeighbors::Face || o.neighbors>FastSweepingNeighbors::FaceEdgeVertex ||
      !o.iterations || o.iterations>32 || !o.dilation || o.dilation>64 ||
      o.topologyHalfWidth<3 || o.topologyHalfWidth>16 || o.topologyClosing>8 || o.topologyDilation>8 || o.topologySmoothing>8 ||
      !o.maxWorkItems || o.maxWorkItems>10'000'000'000ull || !std::isfinite(o.isoValue) ||
      !p.maxVoxelCount || !p.maxInputBytes || p.gridName.empty() || p.gridName.size()>(extension?246:256) || p.gridName.find('\0')!=std::string::npos)
    throw std::invalid_argument("invalid fast sweeping operation/options/output name/budgets");
  if(output.empty() || std::filesystem::exists(output) || !std::filesystem::is_directory(output.has_parent_path()?output.parent_path():std::filesystem::path(".")))
    throw std::invalid_argument("fast sweeping requires a new output in an existing directory");
  if(operation==FastSweepingOperation::MaskSdf && o.mask.empty()) throw std::invalid_argument("MaskSdf requires a mask file");
  if(o.mask.empty() && !o.maskGrid.empty()) throw std::invalid_argument("mask selector requires a mask file");
  if((operation==FastSweepingOperation::DilateSdf || operation==FastSweepingOperation::TopologyToSdf) && !o.mask.empty())
    throw std::invalid_argument("band dilation/topology conversion do not accept a restriction mask");
  if(!extension && operation!=FastSweepingOperation::DilateSdf && o.side!=FastSweepingSide::All)
    throw std::invalid_argument("side selection requires band dilation or field extension");
  if((operation==FastSweepingOperation::DilateSdf || operation==FastSweepingOperation::MaskSdf || operation==FastSweepingOperation::TopologyToSdf) && o.isoValue!=0)
    throw std::invalid_argument("isovalue applies only to fog conversion/reinitialization/field extension");
}
NativeResult topology(const openvdb::GridBase &source,const FastSweepingOptions &o,Budget &b,double h) {
  auto mask=activityMask(source);
  if(mask->tree().hasActiveTiles() && !o.voxelizeActiveTiles) throw std::invalid_argument("topology active tiles require explicit voxelizeActiveTiles");
  const auto growth=o.topologyHalfWidth+o.topologyClosing+o.topologyDilation+4*o.topologySmoothing+4;
  domain(source.evalActiveVoxelBoundingBox(),growth,b);
  b.items(mul(source.activeVoxelCount(),uint64_t(256)*(o.topologyHalfWidth+o.topologySmoothing+o.topologyClosing+o.topologyDilation+1),o.maxWorkItems,"topology conversion passes exceed maxWorkItems"));
  if(o.voxelizeActiveTiles) mask->tree().voxelizeActiveTiles(false);
  auto result=openvdb::tools::topologyToLevelSet(*mask,int(o.topologyHalfWidth),int(o.topologyClosing),int(o.topologyDilation),int(o.topologySmoothing));
  if(!result || result->empty()) throw std::runtime_error("topology conversion produced an empty distance field");
  const float maximum=std::numeric_limits<float>::max();
  for(auto it=result->beginValueAll();it;++it) {
    b.items(1);const double value=double(*it)*h;
    if(!std::isfinite(value) || std::abs(value)>maximum) throw std::invalid_argument("world topology distances exceed Float precision range");it.setValue(float(value));
  }
  result->tree().root().setBackground(float(result->background()*h),false);
  result->insertMeta(source);result->setGridClass(openvdb::GRID_LEVEL_SET);
  if(result->activeVoxelCount()>b.p.maxVoxelCount || result->tree().leafCount()*512>b.p.maxVoxelCount || result->memUsage()>b.projected)
    throw std::runtime_error("topology result exceeded preflight domain/tree limits");
  return {result,{},0,0};
}
FastSweepingResult execute(const std::filesystem::path &input,const std::filesystem::path &field,
    const std::filesystem::path &output,FastSweepingOperation operation,bool extension,bool inputFog,
    const FastSweepingOptions &o,const std::string &selection,const std::string &fieldSelection,const ProcessingOptions &p) {
  validate(o,p,operation,extension,output);openvdb::initialize();tbb::task_arena arena(1);
  return arena.execute([&] {
    Temporary temp;Budget budget{p,o};openvdb::MetaMap fileMeta;
    auto source=load(input,temp.directory/"source.vdb",selection,operation==FastSweepingOperation::TopologyToSdf?Role::Topology:Role::Scalar,budget,&fileMeta);
    const double h=uniformMap(*source);openvdb::GridBase::Ptr maskSource,fieldSource;openvdb::MaskGrid::Ptr mask;
    if(!o.mask.empty()) {
      maskSource=load(o.mask,temp.directory/"mask.vdb",o.maskGrid,Role::Mask,budget);sameMap(*source,*maskSource);
      if(maskSource->baseTree().activeTileCount()!=0 && !o.voxelizeActiveTiles) throw std::invalid_argument("mask active tiles require explicit voxelizeActiveTiles");
      mask=activityMask(*maskSource);
    }
    if(extension) {fieldSource=load(field,temp.directory/"field.vdb",fieldSelection,Role::Field,budget);sameMap(*source,*fieldSource);}
    NativeResult native;
    if(operation==FastSweepingOperation::TopologyToSdf) native=topology(*source,o,budget,h);
    else if(source->isType<openvdb::FloatGrid>()) native=runFloat(*source,fieldSource.get(),mask.get(),operation,inputFog,o,budget,h);
    else native=runDouble(*source,fieldSource.get(),mask.get(),operation,inputFog,o,budget,h);
    clean(*native.distance,p.gridName,*source);native.distance->setGridClass(openvdb::GRID_LEVEL_SET);
    native.distance->setVectorType(openvdb::VEC_INVARIANT);native.distance->setIsInWorldSpace(true);
    openvdb::GridPtrVec outputGrids{native.distance};
    FastSweepingResult report;report.grid={p.gridName,"level set",native.distance->activeVoxelCount(),h};
    report.inputActiveVoxelCount=source->activeVoxelCount();report.boundaryVoxelCount=native.boundary;report.sweepingVoxelCount=native.sweeping;
    if(native.extension) {
      clean(*native.extension,p.gridName+"_extension",*fieldSource);native.extension->setGridClass(openvdb::GRID_UNKNOWN);
      outputGrids.push_back(native.extension);report.extensionGrid={native.extension->getName(),"unknown",native.extension->activeVoxelCount(),h};
      report.diagnostics.emplace_back("extension uses native trilinear boundary samples and fixed eight-direction iterations; selected-side exclusions retain original field values, including boundary voxels");
    }
    report.workItems=budget.work;
    report.diagnostics.emplace_back("unit-index numerical solve; exact source affine coefficients restored; distances scaled to native world units");
    report.diagnostics.emplace_back("finite upwind approximation on the selected active domain; fixed iteration limit does not certify convergence or closure/uniform band width");
    report.diagnostics.emplace_back("shared conservative dense-domain/tree/work caps; full precision ZIP; file/grid metadata retained except regenerated statistics/provenance");
    budget.check();openvdb::io::File writer((temp.directory/"result.vdb").string());writer.setCompression(openvdb::io::COMPRESS_ZIP);writer.write(outputGrids,fileMeta);writer.close();
    copy(temp.directory/"result.vdb",output,p.maxInputBytes);
    return report;
  });
}
#endif
} // namespace
FastSweepingResult WriteFastSwept(const std::filesystem::path &input,const std::filesystem::path &output,
    FastSweepingOperation operation,const FastSweepingOptions &o,const std::string &selection,const ProcessingOptions &p) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  return execute(input,{},output,operation,false,operation==FastSweepingOperation::FogToSdf,o,selection,{},p);
#else
  (void)input;(void)output;(void)operation;(void)o;(void)selection;(void)p;unavailable();
#endif
}
FastSweepingResult WriteFastExtendedField(const std::filesystem::path &input,const std::filesystem::path &field,
    const std::filesystem::path &output,bool inputFog,const FastSweepingOptions &o,const std::string &selection,
    const std::string &fieldSelection,const ProcessingOptions &p) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  return execute(input,field,output,inputFog?FastSweepingOperation::FogToSdf:FastSweepingOperation::ReinitializeSdf,true,inputFog,o,selection,fieldSelection,p);
#else
  (void)input;(void)field;(void)output;(void)inputFog;(void)o;(void)selection;(void)fieldSelection;(void)p;unavailable();
#endif
}
} // namespace container::geometry::vdb
