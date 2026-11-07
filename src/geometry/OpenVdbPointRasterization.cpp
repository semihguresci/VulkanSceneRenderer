#include "Container/geometry/OpenVdbPointRasterization.h"
#include "OpenVdbProvenance.h"
#include "Container/geometry/OpenVdbParticles.h"
#include "Container/geometry/OpenVdbGridSelection.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
#include "OpenVdbFileUtils.h"
#include <openvdb/openvdb.h>
#include <openvdb/points/PointDataGrid.h>
#include <tbb/task_arena.h>
#include <array>
#include <atomic>
#include <chrono>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#endif

namespace container::geometry::vdb {
namespace {
[[noreturn]] void unavailable() {
  throw std::runtime_error("OpenVDB point rasterization is unavailable; configure with -DENABLE_OPENVDB=ON.");
}
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
constexpr long double Fixed=4.0L*1024*1024;
using V=openvdb::Vec3d;
void budget(long double bytes,const ProcessingOptions& p,const char* reason) {
  if (!std::isfinite(bytes) || bytes>p.maxInputBytes) throw std::invalid_argument(reason);
}
ProcessingOptions reduced(const ProcessingOptions& p,long double reserve) {
  const long double rounded=std::ceil(reserve);
  if (!(rounded>=0) || rounded>=0x1p64 || rounded>=static_cast<long double>(p.maxInputBytes))
    throw std::invalid_argument("retained point rasterization data exhausts the memory budget");
  auto result=p; result.maxInputBytes-=static_cast<uintmax_t>(rounded); return result;
}
long double metadataBytes(const openvdb::MetaMap& map) {
  long double bytes=sizeof(openvdb::MetaMap);
  for (auto it=map.beginMeta();it!=map.endMeta();++it)
    bytes+=2.0L*(it->first.size()+it->second->typeName().size()+it->second->size())+256;
  return bytes;
}
void options(const std::filesystem::path& output,const ProcessingOptions& p,const PointOptions& points) {
  if (!p.maxVoxelCount || !p.maxInputBytes || !points.maxPoints)
    throw std::invalid_argument("point rasterization budgets must be positive");
  if (!std::isfinite(p.voxelSize) || p.voxelSize<1.5e-5 || p.voxelSize>1e6)
    throw std::invalid_argument("point rasterization voxel size must be in [1.5e-5,1e6]");
  if (p.gridName.empty() || p.gridName.size()>256)
    throw std::invalid_argument("point rasterization output name must contain 1..256 bytes");
  if (output.empty() || std::filesystem::exists(output)) throw std::invalid_argument("point rasterization output must be a new nonempty path");
  if (output.has_parent_path() && !std::filesystem::is_directory(output.parent_path()))
    throw std::invalid_argument("point rasterization output directory does not exist");
}
struct Temporary {
  std::filesystem::path directory,snapshot,surface;
  explicit Temporary(const std::filesystem::path& output) {
    static std::atomic_uint64_t serial{0};
    const auto parent=output.has_parent_path()?output.parent_path():std::filesystem::path(".");
    for (unsigned n=0;n<64;++n) {
      directory=parent/(".container-point-raster-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+"-"+std::to_string(++serial));
      if (std::filesystem::create_directory(directory)) {snapshot=directory/"source.vdb"; surface=directory/"surface.vdb"; return;}
    }
    throw std::runtime_error("cannot create private point rasterization directory");
  }
  ~Temporary() {std::error_code ignored; std::filesystem::remove(snapshot,ignored); std::filesystem::remove(surface,ignored); std::filesystem::remove(directory,ignored);}
};
uintmax_t snapshot(const std::filesystem::path& source,const std::filesystem::path& target,const ProcessingOptions& p) {
  if (!std::filesystem::is_regular_file(source)) throw std::invalid_argument("point rasterization input is not a regular file");
  const auto encoded=std::filesystem::file_size(source);
  budget(encoded+Fixed,p,"point rasterization encoded input exceeds memory budget");
  std::ifstream input(source,std::ios::binary); std::ofstream output(target,std::ios::binary|std::ios::out|std::ios::noreplace);
  if (!input || !output) throw std::runtime_error("cannot open private point rasterization snapshot");
  std::array<char,64*1024> buffer{}; uintmax_t remaining=encoded;
  while (remaining) {
    const auto count=static_cast<std::streamsize>(std::min<uintmax_t>(remaining,buffer.size()));
    input.read(buffer.data(),count);
    if (input.gcount()!=count) throw std::runtime_error("point source changed size while snapshotting");
    output.write(buffer.data(),count); remaining-=uintmax_t(count);
  }
  output.close();
  if (!output || input.bad() || input.peek()!=std::char_traits<char>::eof())
    throw std::runtime_error("point source changed or failed while snapshotting");
  return encoded;
}
struct Source {openvdb::GridBase::Ptr metadata; std::vector<PointRecord> records; uintmax_t encoded;};
Source load(const Temporary& temporary,const std::string& selector,const ProcessingOptions& p,const PointOptions& points,uintmax_t encoded) {
  detail::RequireSeekableFile(temporary.snapshot);
  openvdb::io::File file(temporary.snapshot.string()); file.open(false);
  {
    std::set<std::string> names; long double bytes=encoded+Fixed;
    for (auto it=file.beginName();it!=file.endName();++it) {
      const auto name=it.gridName(); bytes+=2.0L*name.size()+128; budget(bytes,p,"point rasterization descriptor catalog exceeds memory budget");
      if (!names.insert(name).second) throw std::invalid_argument("point rasterization input has colliding descriptors; rename grids first");
    }
  }
  openvdb::GridBase::Ptr selected; std::string descriptor;
  for (auto it=file.beginName();it!=file.endName();++it) {
    const auto meta=file.readGridMetadata(it.gridName());
    if (!meta->isType<openvdb::points::PointDataGrid>() || !GridSelectionMatches(selector,it.gridName(),meta->getName())) continue;
    if (selected) throw std::invalid_argument("point rasterization grid is ambiguous; use descriptor:<exact name>");
    selected=meta; descriptor=it.gridName(); if (selector.empty()) break;
  }
  file.close();
  if (!selected) throw std::invalid_argument("no matching standard PointDataGrid found for rasterization");
  const auto retained=4*metadataBytes(*selected)+Fixed;
  const auto readOptions=reduced(p,retained);
  auto records=ReadPoints(temporary.snapshot,"descriptor:"+descriptor,readOptions,points);
  budget(encoded+retained+records.size()*static_cast<long double>(sizeof(PointRecord)),p,
         "point rasterization retained metadata/records exceed memory budget");
  return {selected,std::move(records),encoded};
}
void producer(openvdb::GridBase& grid,const Source& source,const std::string& name,openvdb::GridClass kind,const char* method) {
  grid.insertMeta(*source.metadata);
  detail::RemoveBoundaryProjectionMetadata(grid);
  grid.setName(name); grid.setGridClass(kind); grid.setIsInWorldSpace(true); grid.setVectorType(openvdb::VEC_INVARIANT);
  grid.insertMeta("container:point_rasterization",openvdb::StringMetadata(method));
  grid.insertMeta("container:source_point_grid",openvdb::StringMetadata(source.metadata->getName()));
  grid.insertMeta("container:source_point_count",openvdb::Int64Metadata(int64_t(source.records.size())));
}
void initialize() {static std::once_flag once; std::call_once(once,[]{openvdb::initialize();});}

double gap(double value) {
  return std::max(std::abs(std::nextafter(value,INFINITY)-value),std::abs(std::nextafter(value,-INFINITY)-value));
}
struct WeightedCell {openvdb::Coord coordinate; double weight;};
struct Kernel {std::array<WeightedCell,8> values; size_t count=0;};
double product(double a,double b,const char* reason) {
  const double result=a*b;
  if (!std::isfinite(result) || (a!=0 && b!=0 && result==0)) throw std::invalid_argument(reason);
  return result;
}
V targetIndex(const PointRecord& point,const openvdb::math::Transform& transform,double dx) {
  const V world(point.position.data()),index=transform.worldToIndex(world);
  for (int axis=0;axis<3;++axis) {
    if (!std::isfinite(index[axis]) || index[axis]<double(INT32_MIN)+16 || index[axis]>double(INT32_MAX)-16)
      throw std::invalid_argument("point splat exceeds safe target integer coordinates");
    if (!std::isfinite(gap(world[axis])) || gap(world[axis])>dx*1e-4 || gap(index[axis])>1e-4)
      throw std::invalid_argument("point splat lacks world/index precision; recenter or increase target voxel size");
  }
  return index;
}
Kernel kernel(const PointRecord& point,const openvdb::math::Transform& transform,double dx) {
  const auto index=targetIndex(point,transform,dx); openvdb::Coord base; V fraction;
  for (int axis=0;axis<3;++axis) {base[axis]=int32_t(std::floor(index[axis])); fraction[axis]=index[axis]-base[axis];}
  Kernel result; double total=0;
  for (int x=0;x<2;++x) for (int y=0;y<2;++y) for (int z=0;z<2;++z) {
    const double a=x?fraction.x():1-fraction.x(),b=y?fraction.y():1-fraction.y(),c=z?fraction.z():1-fraction.z();
    if (a==0 || b==0 || c==0) continue;
    const double weight=product(product(a,b,"trilinear kernel weight underflows"),c,"trilinear kernel weight underflows");
    result.values[result.count++]={base+openvdb::Coord(x,y,z),weight}; total+=weight;
  }
  if (!(total>0) || !std::isfinite(total)) throw std::invalid_argument("point splat has invalid kernel normalization");
  for (size_t n=0;n<result.count;++n) result.values[n].weight/=total;
  return result;
}
int32_t block(int32_t value,int32_t dimension) {return value>=0?value/dimension:(value+1)/dimension-1;}
openvdb::Coord block(const openvdb::Coord& coordinate,int32_t dimension) {
  return {block(coordinate.x(),dimension),block(coordinate.y(),dimension),block(coordinate.z(),dimension)};
}
template<class Tree> long double treeBytes(size_t leaves,size_t lower,size_t upper) {
  using Leaf=typename Tree::LeafNodeType; using Upper=typename Tree::RootNodeType::ChildNodeType; using Lower=typename Upper::ChildNodeType;
  return leaves*(sizeof(Leaf)+Leaf::NUM_VALUES*sizeof(typename Tree::ValueType)+256.0L)+
    lower*(sizeof(Lower)+256.0L)+upper*(sizeof(Upper)+512.0L);
}
struct Topology {std::set<openvdb::Coord> cells,leaves,lower,upper; long double resident;};
Topology preflight(const Source& source,const openvdb::math::Transform& transform,PointSplatField field,
                    const ProcessingOptions& p,const PointSplatOptions& s) {
  Topology result;
  const int grids=field==PointSplatField::Density?1:2;
  result.resident=source.encoded+(4+3*grids)*metadataBytes(*source.metadata)+source.records.size()*static_cast<long double>(sizeof(PointRecord))+Fixed+
    16.0L*(p.gridName.size()+s.densityGridName.size());
  const auto maxLeaves=p.maxVoxelCount/512+(p.maxVoxelCount%512!=0);
  for (const auto& point:source.records) {
    const auto cells=kernel(point,transform,p.voxelSize);
    for (size_t n=0;n<cells.count;++n) {
      const auto coord=cells.values[n].coordinate;
      // Reserve possible new set nodes and the eventual accumulation map before
      // allocating catalog entries; duplicates do not grow retained storage.
      budget(result.resident+(result.cells.size()+1)*288.0L+
        (result.leaves.size()+result.lower.size()+result.upper.size()+3)*128.0L,p,
        "point splat cell/node catalog exceeds the input-memory budget");
      result.cells.insert(coord); result.leaves.insert(block(coord,8)); result.lower.insert(block(coord,128)); result.upper.insert(block(coord,4096));
      if (result.cells.size()>p.maxVoxelCount/grids || result.leaves.size()>maxLeaves/grids)
        throw std::invalid_argument("point splat aggregate output topology exceeds the voxel budget");
    }
  }
  long double trees=treeBytes<openvdb::DoubleTree>(result.leaves.size(),result.lower.size(),result.upper.size());
  if (field==PointSplatField::Radius) trees*=2;
  if (field==PointSplatField::Velocity) trees+=treeBytes<openvdb::Vec3DTree>(result.leaves.size(),result.lower.size(),result.upper.size());
  const long double catalogs=result.cells.size()*288.0L+(result.leaves.size()+result.lower.size()+result.upper.size())*128.0L;
  budget(result.resident+catalogs+3*trees,p,"point splat accumulation/output workspace exceeds the input-memory budget");
  return result;
}
struct Sum {
  double value=0,correction=0;
  void add(double x) {
    const double adjusted=x-correction,next=value+adjusted;
    if (!std::isfinite(next)) throw std::invalid_argument("point splat mass accumulation overflows");
    correction=(next-value)-adjusted; value=next;
  }
};
struct Cell {Sum mass; V mean{0};};
double volumeScale(double value,double dx,bool divide) {
  if (value==0) return 0;
  int exponent=0,voxelExponent=0;
  const double fraction=std::frexp(value,&exponent),voxel=std::frexp(dx,&voxelExponent);
  const double cube=voxel*voxel*voxel;
  const double result=std::scalbn(divide?fraction/cube:fraction*cube,exponent+(divide?-3:3)*voxelExponent);
  if (!std::isfinite(result) || result==0)
    throw std::invalid_argument("point splat world voxel-volume scaling overflows or underflows; rescale mass/voxel units");
  return result;
}
double mean(double a,double b,double oldMass,double newMass,double total) {
  const double oldWeight=oldMass/total,newWeight=newMass/total;
  if (!(oldWeight>0) || !(newWeight>0) || !std::isfinite(oldWeight) || !std::isfinite(newWeight))
    throw std::invalid_argument("point splat attribute normalization loses kernel mass");
  const double scale=std::max(std::abs(a),std::abs(b));
  if (scale==0) return 0;
  const double left=a/scale,right=b/scale;
  // Keep both mass fractions independently: 1-newWeight can round to zero
  // even when an extreme source value makes the old contribution significant.
  const double unit=std::clamp(std::fma(left,oldWeight,right*newWeight),std::min(left,right),std::max(left,right));
  const double result=unit*scale;
  if (!std::isfinite(result) || (unit!=0 && result==0))
    throw std::invalid_argument("point splat attribute mean overflows or underflows; rescale attribute units");
  return result;
}
PointSplatResult splat(const Source& source,const std::filesystem::path& output,PointSplatField field,
                      const ProcessingOptions& p,const PointSplatOptions& s) {
  const auto transform=openvdb::math::Transform::createLinearTransform(p.voxelSize);
  const double totalMass=source.records.empty()?0:product(s.massPerPoint,double(source.records.size()),"total point mass overflows or underflows");
  const auto topology=preflight(source,*transform,field,p,s);
  std::map<openvdb::Coord,Cell> cells;
  for (const auto& coordinate:topology.cells) cells.emplace(coordinate,Cell{});
  for (const auto& point:source.records) {
    const auto contribution=kernel(point,*transform,p.voxelSize);
    for (size_t n=0;n<contribution.count;++n) {
      const auto& entry=contribution.values[n]; auto& cell=cells.at(entry.coordinate);
      const double mass=product(s.massPerPoint,entry.weight,"positive point kernel mass underflows");
      const double oldMass=cell.mass.value; const bool first=oldMass==0; cell.mass.add(mass);
      if (field!=PointSplatField::Density) {
        for (int axis=0;axis<(field==PointSplatField::Velocity?3:1);++axis) {
          const double value=field==PointSplatField::Velocity?point.velocity[axis]:point.radius;
          cell.mean[axis]=first?value:mean(cell.mean[axis],value,oldMass,mass,cell.mass.value);
        }
      }
    }
  }
  auto density=openvdb::DoubleGrid::create(0.); density->setTransform(detail::CopyTransformExact(*transform));
  openvdb::DoubleGrid::Ptr radius; openvdb::Vec3DGrid::Ptr velocity;
  if (field==PointSplatField::Radius) {radius=openvdb::DoubleGrid::create(0.); radius->setTransform(detail::CopyTransformExact(*transform));}
  if (field==PointSplatField::Velocity) {velocity=openvdb::Vec3DGrid::create(V(0)); velocity->setTransform(detail::CopyTransformExact(*transform));}
  auto densityAccessor=density->getAccessor(); Sum deposited;
  for (const auto& [coordinate,cell]:cells) {
    const double value=volumeScale(cell.mass.value,p.voxelSize,true);
    densityAccessor.setValue(coordinate,value); deposited.add(volumeScale(value,p.voxelSize,false));
    if (radius) radius->tree().setValue(coordinate,cell.mean.x());
    if (velocity) velocity->tree().setValue(coordinate,cell.mean);
  }
  if (totalMass>0 && std::abs(deposited.value/totalMass-1)>1e-12)
    throw std::invalid_argument("point splat integrated density fails relative mass conservation (1e-12); rescale numeric units");
  const bool attributes=field!=PointSplatField::Density;
  producer(*density,source,attributes?s.densityGridName:p.gridName,openvdb::GRID_FOG_VOLUME,"trilinear CIC density");
  openvdb::GridPtrVec grids{density}; openvdb::GridBase::Ptr attribute;
  if (radius) attribute=radius; if (velocity) attribute=velocity;
  if (attribute) {
    producer(*attribute,source,p.gridName,openvdb::GRID_UNKNOWN,field==PointSplatField::Velocity?"trilinear CIC mean velocity":"trilinear CIC mean radius");
    if (velocity) velocity->setVectorType(openvdb::VEC_CONTRAVARIANT_RELATIVE);
    grids.insert(grids.begin(),attribute);
  }
  uint64_t active=0,leaves=0; long double retained=topology.resident+cells.size()*160.0L+
    topology.cells.size()*128.0L+(topology.leaves.size()+topology.lower.size()+topology.upper.size())*128.0L;
  for (auto& grid:grids) {
    grid->insertMeta("container:point_mass",openvdb::DoubleMetadata(s.massPerPoint));
    grid->insertMeta("container:total_point_mass",openvdb::DoubleMetadata(totalMass));
    grid->insertMeta("container:deposited_mass",openvdb::DoubleMetadata(deposited.value));
    grid->insertMeta("container:splat_kernel",openvdb::StringMetadata("collocated trilinear hat; support 8 voxel centers; independent of pscale"));
    grid->insertMeta("container:density_units",openvdb::StringMetadata("point mass / world length^3"));
    retained+=3.0L*grid->memUsage()+3*metadataBytes(*grid); active+=grid->activeVoxelCount(); leaves+=grid->baseTree().leafCount();
    if (grid->activeVoxelCount()!=cells.size() || grid->baseTree().leafCount()!=topology.leaves.size())
      throw std::invalid_argument("point splat output topology differs from its preflight catalog");
  }
  if (active>p.maxVoxelCount || leaves>p.maxVoxelCount/512+(p.maxVoxelCount%512!=0))
    throw std::invalid_argument("point splat actual aggregate output exceeds the voxel budget");
  budget(retained,p,"point splat retained output/metadata exceeds the input-memory budget");
  detail::WriteFileExclusive(output,grids,p.maxInputBytes);
  return {{p.gridName,field==PointSplatField::Density?"fog volume":field==PointSplatField::Velocity?"vector field":"scalar field",
    density->activeVoxelCount(),p.voxelSize},attributes?s.densityGridName:std::string{},source.records.size(),totalMass,deposited.value};
}
#endif
} // namespace

ProcessingResult WritePointsLevelSet(const std::filesystem::path& input,const std::filesystem::path& output,
    const std::string& selector,const ProcessingOptions& p,const PointOptions& points) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  options(output,p,points); initialize(); tbb::task_arena arena(1);
  return arena.execute([&] {
    Temporary temporary(output); const auto encoded=snapshot(input,temporary.snapshot,p);
    const auto source=load(temporary,selector,p,points,encoded);
    const long double resident=encoded+4*metadataBytes(*source.metadata)+source.records.size()*(static_cast<long double>(sizeof(PointRecord))+sizeof(Particle))+Fixed;
    const auto generation=reduced(p,resident);
    const auto transform=openvdb::math::Transform::createLinearTransform(p.voxelSize);
    std::vector<Particle> particles; particles.reserve(source.records.size());
    for (const auto& point:source.records) {(void)targetIndex(point,*transform,p.voxelSize); particles.push_back({point.position,point.radius});}
    const auto result=WriteParticlesLevelSet(particles,temporary.surface,generation,{points.maxPoints});
    const auto encodedSurface=std::filesystem::file_size(temporary.surface);
    budget(resident+encodedSurface,p,"point SDF encoded publication workspace exceeds memory budget");
    detail::RequireSeekableFile(temporary.surface);
    openvdb::io::File file(temporary.surface.string()); file.open(false);
    const auto meta=file.readGridMetadata(p.gridName);
    const auto memory=meta->getMetadata<openvdb::Int64Metadata>(openvdb::GridBase::META_FILE_MEM_BYTES);
    if (!memory || memory->value()<0) throw std::runtime_error("point SDF temporary lacks recorded memory statistics");
    budget(resident+encodedSurface+3.0L*memory->value(),p,"point SDF reload/publication workspace exceeds memory budget");
    auto grid=openvdb::gridPtrCast<openvdb::FloatGrid>(file.readGrid(p.gridName)); file.close();
    producer(*grid,source,p.gridName,openvdb::GRID_LEVEL_SET,"particle sphere SDF using world pscale");
    budget(resident+encodedSurface+3.0L*grid->memUsage()+3*metadataBytes(*grid),p,"point SDF metadata publication exceeds memory budget");
    detail::WriteFileExclusive(output,{grid},p.maxInputBytes); return result;
  });
#else
  (void)input; (void)output; (void)selector; (void)p; (void)points; unavailable();
#endif
}

PointSplatResult WritePointsSplat(const std::filesystem::path& input,const std::filesystem::path& output,PointSplatField field,
    const std::string& selector,const ProcessingOptions& p,const PointSplatOptions& s,const PointOptions& points) {
#if defined(CONTAINER_HAS_OPENVDB) && CONTAINER_HAS_OPENVDB
  options(output,p,points);
  if (field!=PointSplatField::Density && field!=PointSplatField::Velocity && field!=PointSplatField::Radius)
    throw std::invalid_argument("unknown point splat field");
  if (!std::isfinite(s.massPerPoint) || s.massPerPoint<=0) throw std::invalid_argument("point mass must be finite and positive");
  if (field!=PointSplatField::Density && (s.densityGridName.empty() || s.densityGridName.size()>256 || s.densityGridName==p.gridName))
    throw std::invalid_argument("point splat companion density name must contain 1..256 bytes and differ from field name");
  initialize(); tbb::task_arena arena(1);
  return arena.execute([&] {
    Temporary temporary(output); const auto encoded=snapshot(input,temporary.snapshot,p);
    const auto source=load(temporary,selector,p,points,encoded); return splat(source,output,field,p,s);
  });
#else
  (void)input; (void)output; (void)field; (void)selector; (void)p; (void)s; (void)points; unavailable();
#endif
}
} // namespace container::geometry::vdb
