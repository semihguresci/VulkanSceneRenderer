#include "Container/geometry/OpenVdbMeasure.h"
#include "Container/geometry/OpenVdbGridSelection.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <stdexcept>

#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
#include "OpenVdbFileUtils.h"
#include <openvdb/openvdb.h>
#include <openvdb/io/File.h>
#include <openvdb/tools/LevelSetMeasure.h>
#include <tbb/task_arena.h>
#include <mutex>
#include <set>
#endif

namespace container::geometry::vdb {
namespace {
[[noreturn]] void unavailable() {
  throw std::runtime_error("OpenVDB surface measurement is unavailable; configure with -DENABLE_OPENVDB=ON.");
}
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
constexpr long double Fixed = 4.0L*1024*1024;
using Tree = openvdb::DoubleTree;
using Leaf = Tree::LeafNodeType;
using Upper = Tree::RootNodeType::ChildNodeType;
using Lower = Upper::ChildNodeType;
long double metadataBytes(const openvdb::MetaMap& map) {
  long double bytes=sizeof(map);
  for (auto it=map.beginMeta();it!=map.endMeta();++it)
    bytes+=2.0L*(it->first.size()+it->second->typeName().size()+it->second->size())+256;
  return bytes;
}
int64_t block(int32_t value,int64_t dimension) {
  const int64_t v=value;
  return v>=0?v/dimension:(v+1)/dimension-1;
}
long double blocks(const openvdb::CoordBBox& box,int64_t dimension) {
  long double count=1;
  for (int axis=0;axis<3;++axis) count*=block(box.max()[axis],dimension)-block(box.min()[axis],dimension)+1;
  return count;
}
long double treeBytes(const openvdb::CoordBBox& box) {
  return blocks(box,Leaf::DIM)*(sizeof(Leaf)+Leaf::NUM_VALUES*sizeof(double)+256.0L)+
    blocks(box,Lower::DIM)*(sizeof(Lower)+256.0L)+blocks(box,Upper::DIM)*(sizeof(Upper)+512.0L);
}
openvdb::GridBase::Ptr read(const std::filesystem::path& input,const std::string& selection,
                            const ProcessingOptions& p,uintmax_t& encoded) {
  if (!p.maxVoxelCount || !p.maxInputBytes) throw std::invalid_argument("measurement voxel/memory budgets must be positive");
  if (!std::filesystem::is_regular_file(input)) throw std::invalid_argument("measurement input is not a regular file");
  encoded=std::filesystem::file_size(input);
  if (static_cast<long double>(encoded)+Fixed>p.maxInputBytes)
    throw std::invalid_argument("measurement encoded input exceeds the input-memory budget");
  detail::RequireSeekableFile(input);
  openvdb::io::File file(input.string()); file.open(false);
  // Check the complete descriptor catalog before named metadata lookups: some
  // duplicate display names can alias literal descriptor-like names.
  std::set<std::string> descriptors;
  long double catalog=0;
  for (auto it=file.beginName();it!=file.endName();++it) {
    const std::string name=it.gridName(); catalog+=name.size()+128;
    if (encoded+catalog+Fixed>p.maxInputBytes) throw std::invalid_argument("measurement descriptor catalog exceeds memory budget");
    if (!descriptors.insert(name).second) throw std::invalid_argument("measurement input has colliding grid descriptors; rename the grids before use");
  }
  std::string chosen; bool found=false;
  for (auto it=file.beginName();it!=file.endName();++it) {
    const auto meta=file.readGridMetadata(it.gridName());
    if (!GridSelectionMatches(selection,it.gridName(),meta->getName()) ||
        (!meta->isType<openvdb::FloatGrid>() && !meta->isType<openvdb::DoubleGrid>()) ||
        meta->getGridClass()!=openvdb::GRID_LEVEL_SET) continue;
    if (found) throw std::invalid_argument("measurement grid name is ambiguous; use descriptor:<exact descriptor name>");
    const auto count=meta->getMetadata<openvdb::Int64Metadata>(openvdb::GridBase::META_FILE_VOXEL_COUNT);
    const auto memory=meta->getMetadata<openvdb::Int64Metadata>(openvdb::GridBase::META_FILE_MEM_BYTES);
    if (!count || !memory || count->value()<0 || memory->value()<0)
      throw std::invalid_argument("measurement requires recorded nonnegative file_mem_bytes and file_voxel_count; rewrite with grid statistics enabled");
    if (uint64_t(count->value())>p.maxVoxelCount ||
        encoded+catalog+memory->value()+3*metadataBytes(*meta)+Fixed>p.maxInputBytes)
      throw std::invalid_argument("measurement recorded topology exceeds voxel/input-memory budget");
    chosen=it.gridName(); found=true;
    if (selection.empty()) break;
  }
  if (!found) throw std::invalid_argument("no matching Float/Double level set found for measurement");
  auto grid=file.readGrid(chosen); file.close(); return grid;
}

struct Geometry {
  double dx;
  openvdb::Coord offset;
  openvdb::CoordBBox localBounds;
  long double workspace;
};
template<class Grid> Geometry validate(const Grid& grid,const ProcessingOptions& p,uintmax_t encoded) {
  if (grid.getGridClass()!=openvdb::GRID_LEVEL_SET || !std::isfinite(grid.background()) || grid.background()<=0)
    throw std::invalid_argument("measurement requires a positive-background finite signed-distance level set");
  if (!grid.transform().isLinear()) throw std::invalid_argument("measurement requires a uniform orthogonal affine transform");
  const auto map=grid.transform().baseMap();
  const auto origin=map->applyMap(openvdb::Vec3d(0));
  std::array<openvdb::Vec3d,3> basis;
  std::array<double,3> lengths;
  for (int axis=0;axis<3;++axis) {
    if (!std::isfinite(origin[axis])) throw std::invalid_argument("measurement transform origin must be finite");
    openvdb::Vec3d unit(0); unit[axis]=1; basis[axis]=map->applyJacobian(unit);
    lengths[axis]=std::hypot(basis[axis].x(),basis[axis].y(),basis[axis].z());
    if (!std::isfinite(lengths[axis]) || lengths[axis]<=0)
      throw std::invalid_argument("measurement transform must be finite and nonsingular");
    basis[axis]/=lengths[axis];
  }
  const double dx=lengths[0];
  constexpr double tolerance=1e-6;
  if (!grid.hasUniformVoxels() || std::abs(lengths[1]/dx-1)>tolerance || std::abs(lengths[2]/dx-1)>tolerance ||
      std::abs(basis[0].dot(basis[1]))>tolerance || std::abs(basis[0].dot(basis[2]))>tolerance ||
      std::abs(basis[1].dot(basis[2]))>tolerance)
    throw std::invalid_argument("measurement requires uniform orthogonal voxels; resample the input first");
  if (dx<1e-6 || dx>1e6) throw std::invalid_argument("measurement source voxel size must be in [1e-6,1e6]");
  const double band=double(grid.background())/dx;
  if (!std::isfinite(band) || band<3-tolerance || band>64+tolerance)
    throw std::invalid_argument("measurement requires a symmetric narrow-band half width in [3,64] voxels");
  if (grid.empty()) throw std::invalid_argument("cannot measure an empty level set");
  if (grid.tree().hasActiveTiles()) throw std::invalid_argument("measurement requires active voxels without active tiles");
  const auto maxLeaves=p.maxVoxelCount/Leaf::NUM_VALUES+(p.maxVoxelCount%Leaf::NUM_VALUES!=0);
  if (grid.activeVoxelCount()>p.maxVoxelCount || grid.tree().leafCount()>maxLeaves)
    throw std::invalid_argument("measurement source topology exceeds the voxel budget");
  const long double resident=encoded+static_cast<long double>(grid.memUsage())+3*metadataBytes(grid)+Fixed;
  if (resident>p.maxInputBytes) throw std::invalid_argument("measurement loaded source exceeds the input-memory budget");
  auto bounds=grid.evalActiveVoxelBoundingBox();
  const double bg=grid.background();
  for (auto it=grid.cbeginValueAll();it;++it) {
    const double value=*it;
    if (!std::isfinite(value)) throw std::invalid_argument("measurement level set contains a non-finite scalar value");
    if (std::abs(value)>bg*(1+tolerance) || (!it.isValueOn() && std::abs(std::abs(value)-bg)>bg*tolerance))
      throw std::invalid_argument("measurement requires symmetric inactive band values and active distances inside the narrow band");
    if (*it!=grid.background()) bounds.expand(it.getBoundingBox());
  }
  // Select the center from meaningful field values rather than leaf origins:
  // integer-translated copies can have different leaf alignment.
  const auto fieldBounds=bounds;
  for (auto leaf=grid.tree().cbeginLeaf();leaf;++leaf) bounds.expand(leaf->getNodeBoundingBox());
  openvdb::Coord offset;
  for (int axis=0;axis<3;++axis) {
    const int64_t minimum=bounds.min()[axis],maximum=bounds.max()[axis];
    if (minimum<std::numeric_limits<int32_t>::min()+16LL || maximum>std::numeric_limits<int32_t>::max()-16LL)
      throw std::invalid_argument("measurement source coordinates leave insufficient stencil halo");
    const int64_t fieldMinimum=fieldBounds.min()[axis],fieldMaximum=fieldBounds.max()[axis];
    offset[axis]=int32_t(fieldMinimum+(fieldMaximum-fieldMinimum)/2);
  }
  auto local=bounds; local.translate(-offset); local.expand(2);
  if (blocks(local,1)>p.maxVoxelCount || blocks(local,Leaf::DIM)>maxLeaves)
    throw std::invalid_argument("measurement padded bounding/stored topology exceeds the voxel budget");
  // Full translated bounds cover leaf splitting at any integer recentering.
  // Tree construction, leaf manager, two Double reduction buffers, sort scratch
  // and accessors are accounted before allocating the normalized grid.
  const long double workspace=resident+2*treeBytes(local)+blocks(local,Leaf::DIM)*256;
  if (!std::isfinite(workspace) || workspace>p.maxInputBytes)
    throw std::invalid_argument("measurement reconstruction/integration workspace exceeds the input-memory budget");
  return {dx,offset,local,workspace};
}
template<class Grid> SurfaceMeasurements measure(const Grid& source,const ProcessingOptions& p,uintmax_t encoded) {
  const auto geometry=validate(source,p,encoded);
  auto local=openvdb::DoubleGrid::create(double(source.background())/geometry.dx);
  local->setGridClass(openvdb::GRID_LEVEL_SET); local->setTransform(openvdb::math::Transform::createLinearTransform(1.));
  // Tiles first: tree.fill can invalidate cached leaf accessors. Skip implicit
  // positive exterior tiles; preserve every negative interior tile and voxel.
  for (auto it=source.cbeginValueAll();it;++it) {
    if (it.isVoxelValue() || *it==source.background()) continue;
    auto box=it.getBoundingBox(); box.translate(-geometry.offset);
    local->tree().fill(box,double(*it)/geometry.dx,false);
  }
  auto accessor=local->getAccessor();
  bool crossing=false;
  for (auto it=source.cbeginValueAll();it;++it) {
    if (!it.isVoxelValue() || (!it.isValueOn() && *it==source.background())) continue;
    const auto point=it.getCoord()-geometry.offset; const double value=double(*it)/geometry.dx;
    if (it.isValueOn()) accessor.setValue(point,value); else accessor.setValueOff(point,value);
  }
  const auto reader=local->getConstAccessor();
  for (auto it=local->cbeginValueOn();it && !crossing;++it) {
    for (int axis=0;axis<3 && !crossing;++axis) {
      auto neighbor=it.getCoord(); ++neighbor[axis];
      const double a=*it,b=reader.getValue(neighbor);
      crossing=(a<0 && b>=0)||(a>=0 && b<0);
    }
  }
  if (!crossing) throw std::invalid_argument("measurement level set has no resolved zero crossing");
  if (local->activeVoxelCount()!=source.activeVoxelCount() ||
      local->tree().leafCount()>blocks(geometry.localBounds,Leaf::DIM) ||
      encoded+static_cast<long double>(source.memUsage())+3*metadataBytes(source)+2.0L*local->memUsage()+Fixed>p.maxInputBytes)
    throw std::invalid_argument("measurement normalized topology exceeds its preflight estimate");
  openvdb::tools::LevelSetMeasure<openvdb::DoubleGrid> integral(*local); integral.setGrainSize(0);
  SurfaceMeasurements result;
  result.gridName=source.getName(); result.activeVoxelCount=source.activeVoxelCount(); result.voxelSize=geometry.dx;
  result.area=integral.area(false)*geometry.dx*geometry.dx;
  result.volume=integral.volume(false)*geometry.dx*geometry.dx*geometry.dx;
  result.averageMeanCurvature=integral.avgMeanCurvature(false)/geometry.dx;
  result.totalGaussianCurvature=integral.totGaussianCurvature(false);
  if (!std::isfinite(result.area) || result.area<=0 || !std::isfinite(result.volume) || result.volume<=0 ||
      !std::isfinite(result.averageMeanCurvature) || !std::isfinite(result.totalGaussianCurvature))
    throw std::invalid_argument("measurement produced non-finite or nonpositive area/volume; provide a resolved closed signed-distance surface");
  result.eulerEstimate=result.totalGaussianCurvature/(2*std::numbers::pi);
  const double rounded=std::round(result.eulerEstimate);
  // The positive int64 endpoint rounds to 2^63 in Double (also MSVC long
  // double). Compare against the exactly representable exclusive endpoint.
  if (!std::isfinite(rounded) || rounded < -0x1p63 || rounded >= 0x1p63)
    throw std::invalid_argument("measurement Euler estimate exceeds the supported integer range");
  result.estimatedEulerCharacteristic=static_cast<int64_t>(rounded);
  result.eulerRoundingError=std::abs(result.eulerEstimate-rounded);
  result.topologyDiagnostic=result.eulerRoundingError>.1 ?
    "Curvature-integral Euler estimate is more than 0.1 from an integer; refine the grid before interpreting topology." :
    "Euler is a resolution-dependent curvature-integral estimate, not an exact topology test.";
  return result;
}
#endif
} // namespace

SurfaceMeasurements MeasureLevelSet(const std::filesystem::path& input,const std::string& selection,
                                     const ProcessingOptions& p) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  static std::once_flag once; std::call_once(once,[]{openvdb::initialize();});
  tbb::task_arena arena(1);
  return arena.execute([&] {
    uintmax_t encoded=0; const auto source=read(input,selection,p,encoded);
    if (source->isType<openvdb::FloatGrid>()) return measure(*openvdb::gridPtrCast<openvdb::FloatGrid>(source),p,encoded);
    return measure(*openvdb::gridPtrCast<openvdb::DoubleGrid>(source),p,encoded);
  });
#else
  (void)input; (void)selection; (void)p; unavailable();
#endif
}
} // namespace container::geometry::vdb
